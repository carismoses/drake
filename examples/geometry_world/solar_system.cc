#include "drake/examples/geometry_world/solar_system.h"

#include <memory>
#include <string>
#include <vector>

#include "drake/common/find_resource.h"
#include "drake/geometry/frame_id_vector.h"
#include "drake/geometry/frame_kinematics_vector.h"
#include "drake/geometry/geometry_frame.h"
#include "drake/geometry/geometry_instance.h"
#include "drake/math/axis_angle.h"
#include "drake/systems/framework/continuous_state.h"
#include "drake/systems/framework/discrete_values.h"

namespace drake {
namespace examples {
namespace solar_system {

using Eigen::Vector4d;
using geometry::Cylinder;
using geometry::FrameId;
using geometry::FrameIdVector;
using geometry::FramePoseVector;
using geometry::GeometryFrame;
using geometry::GeometryId;
using geometry::GeometryInstance;
using geometry::GeometrySystem;
using geometry::Mesh;
using geometry::SourceId;
using geometry::Sphere;
using systems::BasicVector;
using systems::Context;
using systems::ContinuousState;
using systems::DiscreteValues;
using std::make_unique;

template <typename T>
SolarSystem<T>::SolarSystem(GeometrySystem<T>* geometry_system) {
  DRAKE_DEMAND(geometry_system);
  source_id_ = geometry_system->RegisterSource("solar_system");
  geometry_id_port_ =
      this->DeclareAbstractOutputPort(&SolarSystem::AllocateFrameIdOutput,
                                      &SolarSystem::CalcFrameIdOutput)
          .get_index();
  geometry_pose_port_ =
      this->DeclareAbstractOutputPort(&SolarSystem::AllocateFramePoseOutput,
                                      &SolarSystem::CalcFramePoseOutput)
          .get_index();

  this->DeclareContinuousState(kBodyCount /* num_q */, kBodyCount /* num_v */,
                               0 /* num_z */);

  AllocateGeometry(geometry_system);
}

template <typename T>
const systems::OutputPort<T>& SolarSystem<T>::get_geometry_id_output_port()
    const {
  return systems::System<T>::get_output_port(geometry_id_port_);
}

template <typename T>
const systems::OutputPort<T>& SolarSystem<T>::get_geometry_pose_output_port()
    const {
  return systems::System<T>::get_output_port(geometry_pose_port_);
}

template <typename T>
void SolarSystem<T>::SetDefaultState(const systems::Context<T>&,
                     systems::State<T>* state) const {
  DRAKE_DEMAND(state != nullptr);
  ContinuousState<T>& xc = state->get_mutable_continuous_state();
  VectorX<T> initial_state;
  initial_state.resize(kBodyCount * 2);
  // clang-format off
  initial_state << 0,               // Earth initial position
                   M_PI / 2,        // moon initial position
                   M_PI / 2,        // Mars initial position
                   0,               // phobos initial position
                   2 * M_PI / 5,    // Earth revolution lasts 5 seconds.
                   2 * M_PI,        // moon revolution lasts 1 second.
                   2 * M_PI / 6,    // Mars revolution lasts 6 seconds.
                   2 * M_PI / 1.1;  // phobos revolution lasts 1.1 seconds.
  // clang-format on
  DRAKE_DEMAND(xc.size() == initial_state.size());
  xc.SetFromVector(initial_state);
  DiscreteValues<T>& xd = state->get_mutable_discrete_state();
  for (int i = 0; i < xd.num_groups(); i++) {
    BasicVector<T>& s = xd.get_mutable_vector(i);
    s.SetFromVector(VectorX<T>::Zero(s.size()));
  }
}

// Registers geometry to form an L-shaped arm onto the given frame. The arm is
// defined as shown below:
//
//                        ◯          ← z = height
//   x = 0                │
//   ↓                    │ height
//   ─────────────────────┘          ← z = 0
//                        ↑
//                        x = length
//
// The arm's horizontal length is oriented with the x-axis. The vertical length
// is oriented with the z-axis. The origin of the arm is defined at the local
// origin, and the top of the arm is positioned at the given height.
template <class ParentId>
void MakeArm(SourceId source_id, ParentId parent_id, double length,
             double height, double radius,
             GeometrySystem<double>* geometry_system) {
  Isometry3<double> arm_pose = Isometry3<double>::Identity();
  // tilt it horizontally
  arm_pose.linear() =
      Eigen::AngleAxis<double>(M_PI / 2, Vector3<double>::UnitY()).matrix();
  arm_pose.translation() << length / 2, 0, 0;
  geometry_system->RegisterGeometry(
      source_id, parent_id,
      make_unique<GeometryInstance>(
          arm_pose, make_unique<Cylinder>(radius, length)));

  Isometry3<double> post_pose = Isometry3<double>::Identity();
  post_pose.translation() << length, 0, height/2;
  geometry_system->RegisterGeometry(
      source_id, parent_id,
      make_unique<GeometryInstance>(
          post_pose, make_unique<Cylinder>(radius, height)));
}

template <typename T>
void SolarSystem<T>::AllocateGeometry(GeometrySystem<T>* geometry_system) {
  body_ids_.reserve(kBodyCount);
  body_offset_.reserve(kBodyCount);
  axes_.reserve(kBodyCount);

  const double orrery_bottom = -1.5;
  const double pipe_radius = 0.05;

  // Allocate the sun.
  // NOTE: we don't store the id of the sun geometry because we have no need
  // for subsequent access (the same is also true for dynamic geometries).
  geometry_system->RegisterAnchoredGeometry(
      source_id_, make_unique<GeometryInstance>(Isometry3<double>::Identity(),
                                                make_unique<Sphere>(1.f)));

  // The fixed post on which Sun sits and around which all planets rotate.
  const double post_height = 1;
  Isometry3<double> post_pose = Isometry3<double>::Identity();
  post_pose.translation() << 0, 0, (orrery_bottom + post_height / 2);
  geometry_system->RegisterAnchoredGeometry(
      source_id_, make_unique<GeometryInstance>(
                      post_pose,
                      make_unique<Cylinder>(pipe_radius, post_height)));

  // Allocate the "celestial bodies": two planets orbiting on different planes,
  // each with a moon.

  // For the full description of the frame labels, see solar_system.h.

  // Earth's frame E lies directly *below* the sun (to account for the
  // orrery arm).
  const double kEarthBottom = orrery_bottom + 0.25;
  Isometry3<double> X_SE{Translation3<double>{0, 0, kEarthBottom}};
  FrameId planet_id = geometry_system->RegisterFrame(
      source_id_, GeometryFrame("Earth", X_SE));
  body_ids_.push_back(planet_id);
  body_offset_.push_back(X_SE);
  axes_.push_back(Vector3<double>::UnitZ());

  // The geometry is displaced from the Earth _frame_ so that it orbits.
  const double kEarthOrbitRadius = 3.0;
  Isometry3<double> X_EGe{
      Translation3<double>{kEarthOrbitRadius, 0, -kEarthBottom}};
  geometry_system->RegisterGeometry(
      source_id_, planet_id,
      make_unique<GeometryInstance>(X_EGe, make_unique<Sphere>(0.25f)));
  // Earth's orrery arm.
  MakeArm(source_id_, planet_id, kEarthOrbitRadius, -kEarthBottom, pipe_radius,
          geometry_system);

  // Luna's frame L is at the center of Earth's geometry (Ge). So, X_EL = X_EGe.
  const Isometry3<double>& X_EL = X_EGe;
  FrameId luna_id = geometry_system->RegisterFrame(
      source_id_, planet_id, GeometryFrame("Luna", X_EL));
  body_ids_.push_back(luna_id);
  body_offset_.push_back(X_EL);
  Vector3<double> plane_normal{1, 1, 1};
  axes_.push_back(plane_normal.normalized());

  // The geometry is displaced from Luna's frame so that it orbits.
  const double kLunaOrbitRadius = 0.35;
  // Pick a position at kLunaOrbitRadius distance from the Earth's origin on
  // the plane _perpendicular_ to the moon's normal (<1, 1, 1>).
  // luna_position.dot(plane_normal) will be zero.
  Vector3<double> luna_position =
      Vector3<double>(-1, 0.5, 0.5).normalized() * kLunaOrbitRadius;
  Isometry3<double> X_LGl{Translation3<double>{luna_position}};
  geometry_system->RegisterGeometry(
      source_id_, luna_id,
      make_unique<GeometryInstance>(X_LGl, make_unique<Sphere>(0.075f)));

  // Mars's frame M lies directly *below* the sun (to account for the orrery
  // arm).
  Isometry3<double> X_SM{Translation3<double>{0, 0, orrery_bottom}};
  planet_id = geometry_system->RegisterFrame(
      source_id_, GeometryFrame("Mars", X_SM));
  body_ids_.push_back(planet_id);
  body_offset_.push_back(X_SM);
  plane_normal << 0, 0.1, 1;
  axes_.push_back(plane_normal.normalized());

  // The geometry is displaced from the Mars _frame_ so that it orbits.
  const double kMarsOrbitRadius = 5.0;
  const double kMarsSize = 0.24;
  Isometry3<double> X_MGm{
      Translation3<double>{kMarsOrbitRadius, 0, -orrery_bottom}};
  GeometryId mars_geometry_id = geometry_system->RegisterGeometry(
      source_id_, planet_id,
      make_unique<GeometryInstance>(X_MGm, make_unique<Sphere>(kMarsSize)));

  std::string rings_absolute_path =
      FindResourceOrThrow("drake/examples/geometry_world/planet_rings.obj");
  Vector3<double> axis = Vector3<double>(1, 1, 1).normalized();
  Isometry3<double> X_GmGr(AngleAxis<double>(M_PI / 3, axis));
  geometry_system->RegisterGeometry(
      source_id_, mars_geometry_id,
      make_unique<GeometryInstance>(
          X_GmGr, make_unique<Mesh>(rings_absolute_path, kMarsSize)));

  // Mars's orrery arm.
  MakeArm(source_id_, planet_id, kMarsOrbitRadius, -orrery_bottom, pipe_radius,
          geometry_system);

  // Phobos's frame P is at the center of Mars's geometry (Gm).
  // So, X_MP = X_MGm. The normal of the plane is negated so it orbits in the
  // opposite direction.
  const Isometry3<double>& X_MP = X_MGm;
  FrameId phobos_id = geometry_system->RegisterFrame(
      source_id_, planet_id, GeometryFrame("phobos", X_MP));
  body_ids_.push_back(phobos_id);
  body_offset_.push_back(X_MP);
  plane_normal << 0, 0, -1;
  axes_.push_back(plane_normal.normalized());

  // The geometry is displaced from the Phobos's frame so that it orbits.
  const double kPhobosOrbitRadius = 0.34;
  Isometry3<double> X_PGp{Translation3<double>{kPhobosOrbitRadius, 0, 0}};
  geometry_system->RegisterGeometry(
      source_id_, phobos_id,
      make_unique<GeometryInstance>(X_PGp, make_unique<Sphere>(0.06f)));

  DRAKE_DEMAND(static_cast<int>(body_ids_.size()) == kBodyCount);
}

template <typename T>
FramePoseVector<T> SolarSystem<T>::AllocateFramePoseOutput(
    const Context<T>&) const {
  DRAKE_DEMAND(source_id_.is_valid());
  DRAKE_DEMAND(static_cast<int>(body_offset_.size()) == kBodyCount);
  // NOTE: We initialize with the translational offset during allocation so that
  // the `CalcFramePoseOutput` need only update the rotational component.
  return FramePoseVector<T>(source_id_, body_offset_);
}

template <typename T>
void SolarSystem<T>::CalcFramePoseOutput(const Context<T>& context,
                                         FramePoseVector<T>* poses) const {
  const BasicVector<T>& state = get_state(context);
  DRAKE_ASSERT(poses->vector().size() == body_ids_.size());
  std::vector<Isometry3<T>>& pose_data = poses->mutable_vector();
  for (size_t i = 0; i < body_ids_.size(); ++i) {
    // Frames only revolve around their origin; it is only necessary to set the
    // rotation value.
    T rot{state[i]};
    pose_data[i].linear() = AngleAxis<T>(rot, axes_[i]).matrix();
  }
}

template <typename T>
FrameIdVector SolarSystem<T>::AllocateFrameIdOutput(const MyContext&) const {
  DRAKE_DEMAND(source_id_.is_valid());
  DRAKE_DEMAND(static_cast<int>(body_offset_.size()) == kBodyCount);
  FrameIdVector ids(source_id_);
  ids.AddFrameIds(body_ids_);
  return ids;
}

template <typename T>
void SolarSystem<T>::CalcFrameIdOutput(const MyContext&, FrameIdVector*) const {
  // NOTE: This only needs to do work if the topology changes. This system makes
  // no topology changes.
}

template <typename T>
void SolarSystem<T>::DoCalcTimeDerivatives(
    const MyContext& context, MyContinuousState* derivatives) const {
  const BasicVector<T>& state = get_state(context);
  BasicVector<T>& derivative_vector = get_mutable_state(derivatives);
  derivative_vector.SetZero();
  derivative_vector.get_mutable_value().head(kBodyCount) =
      state.get_value().tail(kBodyCount);
}

template class SolarSystem<double>;

}  // namespace solar_system
}  // namespace examples
}  // namespace drake
