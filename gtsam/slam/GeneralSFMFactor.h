/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file GeneralSFMFactor.h
 *
 * @brief a general SFM factor with an unknown calibration
 *
 * @date Dec 15, 2010
 * @author Kai Ni
 */

#pragma once

#include <gtsam/geometry/PinholeCamera.h>
#include <gtsam/geometry/Point2.h>
#include <gtsam/geometry/Point3.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/nonlinear/NonlinearFactor.h>
#include <gtsam/linear/HessianFactor.h>
#include <gtsam/linear/NoiseModel.h>
#include <gtsam/base/concepts.h>
#include <gtsam/base/Manifold.h>
#include <gtsam/base/Matrix.h>
#include <gtsam/base/SymmetricBlockMatrix.h>
#include <gtsam/base/types.h>
#include <gtsam/base/Testable.h>
#include <gtsam/base/Vector.h>
#include <gtsam/base/timing.h>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <iostream>
#include <string>

namespace boost {
namespace serialization {
class access;
} /* namespace serialization */
} /* namespace boost */

namespace gtsam {

/**
 * Non-linear factor for a constraint derived from a 2D measurement.
 * The calibration is unknown here compared to GenericProjectionFactor
 * @addtogroup SLAM
 */
template<class CAMERA, class LANDMARK>
class GeneralSFMFactor: public NoiseModelFactor2<CAMERA, LANDMARK> {

  GTSAM_CONCEPT_MANIFOLD_TYPE(CAMERA);
  GTSAM_CONCEPT_MANIFOLD_TYPE(LANDMARK);

  static const int DimC = FixedDimension<CAMERA>::value;
  static const int DimL = FixedDimension<LANDMARK>::value;
  typedef Eigen::Matrix<double, 2, DimC> JacobianC;
  typedef Eigen::Matrix<double, 2, DimL> JacobianL;

protected:

  Point2 measured_; ///< the 2D measurement

public:

  typedef GeneralSFMFactor<CAMERA, LANDMARK> This;///< typedef for this object
  typedef NoiseModelFactor2<CAMERA, LANDMARK> Base;///< typedef for the base class

  // shorthand for a smart pointer to a factor
  typedef boost::shared_ptr<This> shared_ptr;

  /**
   * Constructor
   * @param measured is the 2 dimensional location of point in image (the measurement)
   * @param model is the standard deviation of the measurements
   * @param cameraKey is the index of the camera
   * @param landmarkKey is the index of the landmark
   */
  GeneralSFMFactor(const Point2& measured, const SharedNoiseModel& model, Key cameraKey, Key landmarkKey) :
  Base(model, cameraKey, landmarkKey), measured_(measured) {}

  GeneralSFMFactor():measured_(0.0,0.0) {} ///< default constructor
  GeneralSFMFactor(const Point2 & p):measured_(p) {} ///< constructor that takes a Point2
  GeneralSFMFactor(double x, double y):measured_(x,y) {} ///< constructor that takes doubles x,y to make a Point2

  virtual ~GeneralSFMFactor() {} ///< destructor

  /// @return a deep copy of this factor
  virtual gtsam::NonlinearFactor::shared_ptr clone() const {
    return boost::static_pointer_cast<gtsam::NonlinearFactor>(
        gtsam::NonlinearFactor::shared_ptr(new This(*this)));}

  /**
   * print
   * @param s optional string naming the factor
   * @param keyFormatter optional formatter for printing out Symbols
   */
  void print(const std::string& s = "SFMFactor", const KeyFormatter& keyFormatter = DefaultKeyFormatter) const {
    Base::print(s, keyFormatter);
    measured_.print(s + ".z");
  }

  /**
   * equals
   */
  bool equals(const NonlinearFactor &p, double tol = 1e-9) const {
    const This* e = dynamic_cast<const This*>(&p);
    return e && Base::equals(p, tol) && this->measured_.equals(e->measured_, tol);
  }

  /** h(x)-z */
  Vector evaluateError(const CAMERA& camera, const LANDMARK& point,
      boost::optional<Matrix&> H1=boost::none, boost::optional<Matrix&> H2=boost::none) const {
    try {
      Point2 reprojError(camera.project2(point,H1,H2) - measured_);
      return reprojError.vector();
    }
    catch( CheiralityException& e) {
      if (H1) *H1 = JacobianC::Zero();
      if (H2) *H2 = JacobianL::Zero();
      // TODO warn if verbose output asked for
      return zero(2);
    }
  }

  class BinaryJacobianFactor : public JacobianFactor {
    // Fixed size matrices
    // TODO(frank): implement generic BinaryJacobianFactor<N,M1,M2> next to
    // JacobianFactor

  public:
    /// Constructor
    BinaryJacobianFactor(Key key1, const JacobianC& A1, Key key2, const JacobianL& A2,
        const Vector2& b,
        const SharedDiagonal& model = SharedDiagonal())
    : JacobianFactor(key1, A1, key2, A2, b, model) {}

    // Fixed-size matrix update
    void updateHessian(const FastVector<Key>& infoKeys, SymmetricBlockMatrix* info) const {
      gttic(updateHessian_BinaryJacobianFactor);
      // Whiten the factor if it has a noise model
      const SharedDiagonal& model = get_model();
      if (model && !model->isUnit()) {
        if (model->isConstrained())
        throw std::invalid_argument(
            "BinaryJacobianFactor::updateHessian: cannot update information with "
            "constrained noise model");
        JacobianFactor whitenedFactor = whiten(); // TODO: make BinaryJacobianFactor
        whitenedFactor.updateHessian(infoKeys, info);
      } else {
        // First build an array of slots
        DenseIndex slot1 = Slot(infoKeys, keys_.front());
        DenseIndex slot2 = Slot(infoKeys, keys_.back());
        DenseIndex slotB = info->nBlocks() - 1;

        const Matrix& Ab = Ab_.matrix();
        Eigen::Block<const Matrix,2,DimC> A1(Ab, 0, 0);
        Eigen::Block<const Matrix,2,DimL> A2(Ab, 0, DimC);
        Eigen::Block<const Matrix,2,1> b(Ab, 0, DimC + DimL);

        // We perform I += A'*A to the upper triangle
        (*info)(slot1, slot1).selfadjointView().rankUpdate(A1.transpose());
        (*info)(slot1, slot2).knownOffDiagonal() += A1.transpose() * A2;
        (*info)(slot1, slotB).knownOffDiagonal() += A1.transpose() * b;
        (*info)(slot2, slot2).selfadjointView().rankUpdate(A2.transpose());
        (*info)(slot2, slotB).knownOffDiagonal() += A2.transpose() * b;
        (*info)(slotB, slotB)(0,0) = b.transpose() * b;
      }
    }
  };

  /// Linearize using fixed-size matrices
  boost::shared_ptr<GaussianFactor> linearize(const Values& values) const {
    // Only linearize if the factor is active
    if (!this->active(values)) return boost::shared_ptr<BinaryJacobianFactor>();

    const Key key1 = this->key1(), key2 = this->key2();
    JacobianC H1;
    JacobianL H2;
    Vector2 b;
    try {
      const CAMERA& camera = values.at<CAMERA>(key1);
      const LANDMARK& point = values.at<LANDMARK>(key2);
      Point2 reprojError(camera.project2(point, H1, H2) - measured());
      b = -reprojError.vector();
    } catch (CheiralityException& e) {
      H1.setZero();
      H2.setZero();
      b.setZero();
      // TODO warn if verbose output asked for
    }

    // Whiten the system if needed
    const SharedNoiseModel& noiseModel = this->get_noiseModel();
    if (noiseModel && !noiseModel->isUnit()) {
      // TODO: implement WhitenSystem for fixed size matrices and include
      // above
      H1 = noiseModel->Whiten(H1);
      H2 = noiseModel->Whiten(H2);
      b = noiseModel->Whiten(b);
    }

    if (noiseModel && noiseModel->isConstrained()) {
      using noiseModel::Constrained;
      return boost::make_shared<BinaryJacobianFactor>(
          key1, H1, key2, H2, b,
          boost::static_pointer_cast<Constrained>(noiseModel)->unit());
    } else {
      return boost::make_shared<BinaryJacobianFactor>(key1, H1, key2, H2, b);
    }
  }

  /** return the measured */
  inline const Point2 measured() const {
    return measured_;
  }

private:
  /** Serialization function */
  friend class boost::serialization::access;
  template<class Archive>
  void serialize(Archive & ar, const unsigned int /*version*/) {
    ar & boost::serialization::make_nvp("NoiseModelFactor2",
        boost::serialization::base_object<Base>(*this));
    ar & BOOST_SERIALIZATION_NVP(measured_);
  }
};

template<class CAMERA, class LANDMARK>
struct traits<GeneralSFMFactor<CAMERA, LANDMARK> > : Testable<
    GeneralSFMFactor<CAMERA, LANDMARK> > {
};

/**
 * Non-linear factor for a constraint derived from a 2D measurement.
 * Compared to GeneralSFMFactor, it is a ternary-factor because the calibration is isolated from camera..
 */
template<class CALIBRATION>
class GeneralSFMFactor2: public NoiseModelFactor3<Pose3, Point3, CALIBRATION> {

  GTSAM_CONCEPT_MANIFOLD_TYPE(CALIBRATION);
  static const int DimK = FixedDimension<CALIBRATION>::value;

protected:

  Point2 measured_; ///< the 2D measurement

public:

  typedef GeneralSFMFactor2<CALIBRATION> This;
  typedef PinholeCamera<CALIBRATION> Camera;///< typedef for camera type
  typedef NoiseModelFactor3<Pose3, Point3, CALIBRATION> Base;///< typedef for the base class

  // shorthand for a smart pointer to a factor
  typedef boost::shared_ptr<This> shared_ptr;

  /**
   * Constructor
   * @param measured is the 2 dimensional location of point in image (the measurement)
   * @param model is the standard deviation of the measurements
   * @param poseKey is the index of the camera
   * @param landmarkKey is the index of the landmark
   * @param calibKey is the index of the calibration
   */
  GeneralSFMFactor2(const Point2& measured, const SharedNoiseModel& model, Key poseKey, Key landmarkKey, Key calibKey) :
  Base(model, poseKey, landmarkKey, calibKey), measured_(measured) {}
  GeneralSFMFactor2():measured_(0.0,0.0) {} ///< default constructor

  virtual ~GeneralSFMFactor2() {} ///< destructor

  /// @return a deep copy of this factor
  virtual gtsam::NonlinearFactor::shared_ptr clone() const {
    return boost::static_pointer_cast<gtsam::NonlinearFactor>(
        gtsam::NonlinearFactor::shared_ptr(new This(*this)));}

  /**
   * print
   * @param s optional string naming the factor
   * @param keyFormatter optional formatter useful for printing Symbols
   */
  void print(const std::string& s = "SFMFactor2", const KeyFormatter& keyFormatter = DefaultKeyFormatter) const {
    Base::print(s, keyFormatter);
    measured_.print(s + ".z");
  }

  /**
   * equals
   */
  bool equals(const NonlinearFactor &p, double tol = 1e-9) const {
    const This* e = dynamic_cast<const This*>(&p);
    return e && Base::equals(p, tol) && this->measured_.equals(e->measured_, tol);
  }

  /** h(x)-z */
  Vector evaluateError(const Pose3& pose3, const Point3& point, const CALIBRATION &calib,
      boost::optional<Matrix&> H1=boost::none,
      boost::optional<Matrix&> H2=boost::none,
      boost::optional<Matrix&> H3=boost::none) const
  {
    try {
      Camera camera(pose3,calib);
      Point2 reprojError(camera.project(point, H1, H2, H3) - measured_);
      return reprojError.vector();
    }
    catch( CheiralityException& e) {
      if (H1) *H1 = zeros(2, 6);
      if (H2) *H2 = zeros(2, 3);
      if (H3) *H3 = zeros(2, DimK);
      std::cout << e.what() << ": Landmark "<< DefaultKeyFormatter(this->key2())
      << " behind Camera " << DefaultKeyFormatter(this->key1()) << std::endl;
    }
    return zero(2);
  }

  /** return the measured */
  inline const Point2 measured() const {
    return measured_;
  }

private:
  /** Serialization function */
  friend class boost::serialization::access;
  template<class Archive>
  void serialize(Archive & ar, const unsigned int /*version*/) {
    ar & boost::serialization::make_nvp("NoiseModelFactor3",
        boost::serialization::base_object<Base>(*this));
    ar & BOOST_SERIALIZATION_NVP(measured_);
  }
};

template<class CALIBRATION>
struct traits<GeneralSFMFactor2<CALIBRATION> > : Testable<
    GeneralSFMFactor2<CALIBRATION> > {
};

} //namespace
