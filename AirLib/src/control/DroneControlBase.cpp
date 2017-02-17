// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

//in header only mode, control library is not available
#ifndef AIRLIB_HEADER_ONLY
//if using Unreal Build system then include precompiled header file first
#ifdef AIRLIB_PCH
#include "AirSim.h"
#endif

#include <functional>
#include <exception>
#include <vector>
#include <iostream>
#include <fstream>
#include "control/DroneControlBase.hpp"

namespace msr { namespace airlib {

float DroneControlBase::getAutoLookahead(float velocity, float adaptive_lookahead,
        float max_factor, float min_factor)
{
    //if auto mode requested for lookahead then calculate based on velocity
    float command_period_dist = velocity * getCommandPeriod();
    float lookahead = command_period_dist * (adaptive_lookahead > 0 ? min_factor : max_factor);
    lookahead = std::max(lookahead, getDistanceAccuracy()*1.5f); //50% more than distance accuracy
    return lookahead;
}

float DroneControlBase::getObsAvoidanceVelocity(float risk_dist, float max_obs_avoidance_vel)
{
    return max_obs_avoidance_vel;
}

void DroneControlBase::setSafetyEval(const shared_ptr<SafetyEval> safety_eval_ptr)
{
    safety_eval_ptr_ = safety_eval_ptr;
}

bool DroneControlBase::moveByAngle(float pitch, float roll, float z, float yaw, float duration
    , CancelableActionBase& cancelable_action)
{
    if (duration <= 0)
        return true;

    return !waitForFunction([&]() {
        return !moveByRollPitchZ(pitch, roll, z, yaw);
    }, duration, cancelable_action);
}

bool DroneControlBase::moveByVelocity(float vx, float vy, float vz, float duration, DrivetrainType drivetrain, const YawMode& yaw_mode,
    CancelableActionBase& cancelable_action)
{
    if (duration <= 0)
        return true;

    YawMode adj_yaw_mode(yaw_mode.is_rate, yaw_mode.yaw_or_rate);
    adjustYaw(vx, vy, drivetrain, adj_yaw_mode);

    return !waitForFunction([&]() {
        return !moveByVelocity(vx, vy, vz, adj_yaw_mode);
    }, duration, cancelable_action);
}

bool DroneControlBase::moveByVelocityZ(float vx, float vy, float z, float duration, DrivetrainType drivetrain, const YawMode& yaw_mode,
    CancelableActionBase& cancelable_action)
{
    if (duration <= 0)
        return false;

    YawMode adj_yaw_mode(yaw_mode.is_rate, yaw_mode.yaw_or_rate);
    adjustYaw(vx, vy, drivetrain, adj_yaw_mode);

    return !waitForFunction([&]() {
        return !moveByVelocityZ(vx, vy, z, adj_yaw_mode);
    }, duration, cancelable_action);

}

bool DroneControlBase::moveOnPath(const vector<Vector3r>& path, float velocity, DrivetrainType drivetrain, const YawMode& yaw_mode,
    float lookahead, float adaptive_lookahead, CancelableActionBase& cancelable_action)
{
    //validate path size
    if (path.size() == 0) {
        Utils::logMessage("moveOnPath terminated because path has no points");
        return true;
    }

    //validate yaw mode
    if (drivetrain == DrivetrainType::ForwardOnly && yaw_mode.is_rate)
        throw std::invalid_argument("Yaw cannot be specified as rate if drivetrain is ForwardOnly");

    //validate and set auto-lookahead value
    float command_period_dist = velocity * getCommandPeriod();
    if (lookahead == 0)
        throw std::invalid_argument("lookahead distance cannot be 0"); //won't allow progress on path
    else if (lookahead > 0) {
        if (command_period_dist > lookahead)
            throw std::invalid_argument(Utils::stringf("lookahead value %f is too small for velocity %f. It must be at least %f", lookahead, velocity, command_period_dist));
        if (getDistanceAccuracy() > lookahead)
            throw std::invalid_argument(Utils::stringf("lookahead value %f is smaller than drone's distance accuracy %f.", lookahead, getDistanceAccuracy()));
    }
    else {
        //if auto mode requested for lookahead then calculate based on velocity
        lookahead = getAutoLookahead(velocity, adaptive_lookahead);
        Utils::logMessage("lookahead = %f, adaptive_lookahead = %f", lookahead, adaptive_lookahead);        
    }

    //add current position as starting point
    vector<Vector3r> path3d;
    vector<PathSegment> path_segs;
    path3d.push_back(getPosition());

    std::ofstream flog;
    if (log_to_file)
        Utils::createLogFile("MoveToPosition", flog);

    Vector3r point;
    float path_length = 0;
    //append the input path and compute segments
    for(uint i = 0; i < path.size(); ++i) {
        point = path.at(i);
        PathSegment path_seg(path3d.at(i), point, velocity, path_length);
        path_length += path_seg.seg_length;
        path_segs.push_back(path_seg);
        path3d.push_back(point);
    }
    //add last segment as zero length segment so we have equal number of segments and points. 
    //path_segs[i] refers to segment that starts at point i
    path_segs.push_back(PathSegment(point, point, velocity, path_length));

    //when path ends, we want to slow down
    float breaking_dist = 0;
    if (velocity > getVehicleParams().breaking_vel) {
        breaking_dist = std::max(velocity * getVehicleParams().vel_to_breaking_dist, getVehicleParams().min_vel_to_breaking_dist);
    }
    //else no need to change velocities for last segments

    //setup current position on path to 0 offset
    PathPosition cur_path_loc, next_path_loc;
    cur_path_loc.seg_index = 0;
    cur_path_loc.offset = 0;
    cur_path_loc.position = path3d[0];

    float lookahead_error = 0;
    Waiter waiter(getCommandPeriod());

    //initialize next path position
    setNextPathPosition(path3d, path_segs, cur_path_loc, lookahead + lookahead_error, next_path_loc);

    while (next_path_loc.seg_index < path_segs.size()-1 || //until we are at the end of the path (last seg is always zero size)
        ((next_path_loc.position - getPosition()).norm() > getDistanceAccuracy())) { //current position is approximately at the last end point

        float seg_velocity = path_segs.at(next_path_loc.seg_index).seg_velocity;
        float path_length_remaining = path_length - path_segs.at(cur_path_loc.seg_index).seg_path_length - cur_path_loc.offset;
        if (path_length_remaining <= breaking_dist) {
            seg_velocity = getVehicleParams().breaking_vel;
            //Utils::logMessage("path_length_remaining = %f, Switched to breaking vel %f", path_length_remaining, seg_velocity);
        }

        //send drone command to get to next lookahead
        moveToPathPosition(next_path_loc.position, seg_velocity, drivetrain, 
            yaw_mode, path_segs.at(cur_path_loc.seg_index).start_z);

        //sleep for rest of the cycle
        if (!waiter.sleep(cancelable_action))
            return false;

        /*  Below, P is previous position on path, N is next goal and C is our current position.

        N
        ^
        |
        |
        |
        C'|---C
        |  /
        | /
        |/
        P

        Note that PC could be at any angle relative to PN, including 0 or -ve. We increase lookahead distance
        by the amount of |PC|. For this, we project PC on to PN to get vector PC' and length of
        CC'is our adaptive lookahead error by which we will increase lookahead distance. 

        For next iteration, we first update our current position by goal_dist and then
        set next goal by the amount lookahead + lookahead_error.

        We need to take care of following cases:

        1. |PN| == 0 => lookahead_error = |PC|, goal_dist = 0
        2. |PC| == 0 => lookahead_error = 0, goal_dist = 0
        3. PC in opposite direction => lookahead_error = |PC|, goal_dist = 0

        One good test case is if C just keeps moving perpendicular to the path (instead of along the path).
        In that case, we expect next goal to come up and down by the amount of lookahead_error. However
        under no circumstances we should go back on the path (i.e. current pos on path can only move forward).
        */

        //how much have we moved towards last goal?
        const Vector3r& goal_vect = next_path_loc.position - cur_path_loc.position;

        float goal_dist;
        if (!goal_vect.isZero()) { //goal can only be zero if we are at the end of path
            const Vector3r& actual_vect = getPosition() - cur_path_loc.position;

            //project actual vector on goal vector
            const Vector3r& goal_normalized = goal_vect.normalized();    
            goal_dist = actual_vect.dot(goal_normalized); //dist could be -ve if drone moves away from goal

                                                            //if adaptive lookahead is enabled the calculate lookahead error (see above fig)
            if (adaptive_lookahead) {
                const Vector3r& actual_on_goal = goal_normalized * goal_dist;
                lookahead_error = (actual_vect - actual_on_goal).norm() * adaptive_lookahead;
            }
        }
        else {
            goal_dist = 0;
            lookahead_error = 0; //this is not really required because we will exit
        }

        // Utils::logMessage("PF: cur=%s, goal_dist=%f, cur_path_loc=%s, next_path_loc=%s, lookahead_error=%f",
        //     VectorMath::toString(getPosition()).c_str(), goal_dist, VectorMath::toString(cur_path_loc.position).c_str(),
        //     VectorMath::toString(next_path_loc.position).c_str(), lookahead_error);

        if (log_to_file)
            flog << cur_path_loc.seg_index << "\t" << cur_path_loc.offset << "\t" << cur_path_loc.position.x() << "\t" << cur_path_loc.position.y() << "\t" << cur_path_loc.position.z() << "\t" << goal_dist << "\t";

        //if drone moved backward, we don't want goal to move backward as well
        //so only climb forward on the path, never back. Also note >= which means
        //we climb path even if distance was 0 to take care of duplicated points on path
        if (goal_dist >= 0) {
            float overshoot = setNextPathPosition(path3d, path_segs, cur_path_loc, goal_dist, cur_path_loc);
            if (overshoot)
                Utils::logMessage("overshoot=%f", overshoot);
        }
        //else
        //    Utils::logMessage("goal_dist was negative: %f", goal_dist);

        //compute next target on path
        setNextPathPosition(path3d, path_segs, cur_path_loc, lookahead + lookahead_error, next_path_loc);

        if (log_to_file) {
            flog << cur_path_loc.seg_index << "\t" << cur_path_loc.offset << "\t" << cur_path_loc.position.x() << "\t" << cur_path_loc.position.y() << "\t" << cur_path_loc.position.z() << "\t" << lookahead  << "\t" << lookahead_error  << "\t";
            flog << next_path_loc.seg_index << "\t" << next_path_loc.offset << "\t" << next_path_loc.position.x() << "\t" << next_path_loc.position.y() << "\t" << next_path_loc.position.z() << std::endl;
        }
    }

    return true;
}

bool DroneControlBase::moveToPosition(float x, float y, float z, float velocity, DrivetrainType drivetrain,
    const YawMode& yaw_mode, float lookahead, float adaptive_lookahead, CancelableActionBase& cancelable_action)
{
    vector<Vector3r> path { Vector3r(x, y, z) };
    return moveOnPath(path, velocity, drivetrain, yaw_mode, lookahead, adaptive_lookahead, cancelable_action);
}

bool DroneControlBase::moveToZ(float z, float velocity, const YawMode& yaw_mode,
    float lookahead, float adaptive_lookahead, CancelableActionBase& cancelable_action)
{
    Vector2r cur_xy = getPositionXY();
    vector<Vector3r> path { Vector3r(cur_xy.x(), cur_xy.y(), z) };
    return moveOnPath(path, velocity, DrivetrainType::MaxDegreeOfFreedome, yaw_mode, lookahead, adaptive_lookahead,
        cancelable_action);
}

bool DroneControlBase::rotateToYaw(float yaw, float margin, CancelableActionBase& cancelable_action)
{
    YawMode yaw_mode(false, VectorMath::normalizeAngleDegrees(yaw));
    Waiter waiter(getCommandPeriod());
    bool is_yaw_reached;
    while ((is_yaw_reached = isYawWithinMargin(yaw, margin)) == false) {
        //DJI has a bug in simulator where getPosition lags behind by as much as 20m which makes below very unsafe
        //if (!moveToPosition(getPosition(), yaw_mode))
        //    return false;
        if (!moveByVelocityZ(0, 0, getZ(), yaw_mode))
            return false;

        if (!waiter.sleep(cancelable_action))
            return false;
    }

    return true;
}

bool DroneControlBase::rotateByYawRate(float yaw_rate, float duration, CancelableActionBase& cancelable_action)
{
    if (duration <= 0)
        return true;

    YawMode yaw_mode(true, yaw_rate);
    Waiter waiter(getCommandPeriod(), duration);
    do {
        //DJI has a bug in simulator where getPosition lags behind by as much as 20m which makes below very unsafe
        //if (!moveToPosition(getPosition(), yaw_mode))
        //    return false;
        if (!moveByVelocityZ(0, 0, getZ(), yaw_mode))
            return false;

    } while (waiter.sleep(cancelable_action) && !waiter.is_timeout());

    return waiter.is_timeout();
}

bool DroneControlBase::hover(CancelableActionBase& cancelable_action)
{
    return moveToZ(getZ(), 0.5f, YawMode{ true,0 }, 1.0f, false, cancelable_action);
}

bool DroneControlBase::moveByVelocity(float vx, float vy, float vz, const YawMode& yaw_mode)
{
    if (safetyCheckVelocity(Vector3r(vx, vy, vz)))
        commandVelocity(vx, vy, vz, yaw_mode);

    return true;
}

bool DroneControlBase::moveByVelocityZ(float vx, float vy, float z, const YawMode& yaw_mode)
{
    if (safetyCheckVelocityZ(vx, vy, z))
        commandVelocityZ(vx, vy, z, yaw_mode);

    return true;
}

bool DroneControlBase::moveToPosition(const Vector3r& dest, const YawMode& yaw_mode)
{
    if (safetyCheckDestination(dest))
        commandPosition(dest.x(), dest.y(), dest.z(), yaw_mode);

    return true;
}

bool DroneControlBase::moveByRollPitchZ(float pitch, float roll, float z, float yaw)
{
    if (safetyCheckVelocity(getVelocity()))
        commandRollPitchZ(pitch, roll, z, yaw);

    return true;
}

bool DroneControlBase::setSafety(SafetyEval::SafetyViolationType enable_reasons, float obs_clearance, SafetyEval::ObsAvoidanceStrategy obs_startegy, 
    float obs_avoidance_vel, const Vector3r& origin, float xy_length, float max_z, float min_z)
{
    if (safety_eval_ptr_ == nullptr)
        throw std::invalid_argument("The setSafety call requires safety_eval_ptr_ to be set first");

    //default strategy is for move. In hover mode we set new strategy temporarily
    safety_eval_ptr_->setSafety(enable_reasons, obs_clearance, obs_startegy, origin, xy_length, max_z, min_z);

    obs_avoidance_vel_ = obs_avoidance_vel;
    Utils::logMessage("obs_avoidance_vel: %f", obs_avoidance_vel_);

    return true;
}

bool DroneControlBase::moveByManual(float vx_max, float vy_max, float z_min, DrivetrainType drivetrain, const YawMode& yaw_mode, float duration, CancelableActionBase& cancelable_action)
{
    const float kMaxMessageAge = 1, kMaxVelocity = 2, trim_duration = 1, kMinCountForTrim = 10, kMaxTrim = 100, kMaxRCValue = 10000;

    if (duration <= 0)
        return true;

    //freeze the quaternion
    Quaternionr starting_quaternion = getOrientation();

    //get trims
    Waiter waiter_trim(getCommandPeriod(), trim_duration);
    RCData rc_data_trims;
    uint count = 0;
    do {

        const RCData rc_data = getRCData();
        if (rc_data.timestamp > 0) {
            rc_data_trims.add(rc_data);
            count++;
        }

    } while (waiter_trim.sleep(cancelable_action) && !waiter_trim.is_timeout());

    if (count < kMinCountForTrim)
        throw MoveException("Cannot compute RC trim because too few readings received");

    //take average
    rc_data_trims.divideBy(static_cast<float>(count));
    if (rc_data_trims.isAnyMoreThan(kMaxTrim))
        throw MoveException(Utils::stringf("RC trims does not seem to be valid: %s", rc_data_trims.toString().c_str()));

    Utils::logMessage("RCData Trims: %s", rc_data_trims.toString().c_str());

    Waiter waiter(getCommandPeriod(), duration);
    do {

        RCData rc_data = getRCData();
        double age = timestampNow() - rc_data.timestamp;
        if (age <= kMaxMessageAge) {
            rc_data.subtract(rc_data_trims);

            //convert RC commands to velocity vector
            const Vector3r vel_word(rc_data.pitch * vy_max/kMaxRCValue, rc_data.roll  * vx_max/kMaxRCValue, 0);
            Vector3r vel_body = VectorMath::transformToBodyFrame(vel_word, starting_quaternion, true);

            //find yaw as per terrain and remote setting
            YawMode adj_yaw_mode(yaw_mode.is_rate, yaw_mode.yaw_or_rate);
            adj_yaw_mode.yaw_or_rate += rc_data.yaw * 100.0f/kMaxRCValue;
            adjustYaw(vel_body, drivetrain, adj_yaw_mode);

            //execute command
            try {
                float vz = (rc_data.throttle / kMaxRCValue) * z_min + getZ();
                moveByVelocityZ(vel_body.x(), vel_body.y(), vz, adj_yaw_mode);
            }
            catch(const DroneControlBase::UnsafeMoveException& ex) {
                Utils::logError("Safety violation: %s", ex.result.message.c_str());
            }
        }
        else
            Utils::logMessage("RCData had too old timestamp: %f", age);

    } while (waiter.sleep(cancelable_action) && !waiter.is_timeout());

    return waiter.is_timeout();
}

Vector2r DroneControlBase::getPositionXY()
{
    const Vector3r& cur_loc3 = getPosition();
    Vector2r cur_loc(cur_loc3.x(), cur_loc3.y());
    return cur_loc;
}

float DroneControlBase::getZ()
{
    return getPosition().z();
}

bool DroneControlBase::waitForFunction(WaitFunction function, float max_wait_seconds, CancelableActionBase& cancelable_action)
{
    if (max_wait_seconds < 0)
    {
        return false;
    }
    bool found = false;
    Waiter waiter(getCommandPeriod(), max_wait_seconds);
    do {
        if (function()) {
            found = true;
            break;
        }
    }
    while (waiter.sleep(cancelable_action) && !waiter.is_timeout());
    return found;
}

bool DroneControlBase::waitForZ(float max_wait_seconds, float z, float margin, CancelableActionBase& cancelable_action)
{
    float cur_z = FLT_MAX;
    if (!waitForFunction([&]() {
        cur_z = getZ();
        return (std::abs(cur_z - z) <= margin);
    }, max_wait_seconds, cancelable_action))
    {
        //Only raise exception is time out occurred. If preempted then return status.
        throw MoveException(Utils::stringf("Drone hasn't came to expected z of %f within time %f sec within error margin %f (current z = %f)",
            z, max_wait_seconds, margin, cur_z));
    }
    return true;
}


bool DroneControlBase::emergencyManeuverIfUnsafe(const SafetyEval::EvalResult& result)
{
    if (!result.is_safe) {
        if (result.reason == SafetyEval::SafetyViolationType_::Obstacle) {
            //are we supposed to do EM?
            if (safety_eval_ptr_->getObsAvoidanceStrategy() != SafetyEval::ObsAvoidanceStrategy::RaiseException) {
                //get suggested velocity vector
                Vector3r avoidance_vel = getObsAvoidanceVelocity(result.cur_risk_dist, obs_avoidance_vel_) * result.suggested_vec;

                //use the unchecked command
                commandVelocityZ(avoidance_vel.x(), avoidance_vel.y(), getZ(), YawMode::Zero());

                //tell caller not to execute planned command
                return false;
            }
            //other wise throw exception
        }
        //otherwise there is some other reason why we are in unsafe situation
        //send last command to come to full stop
        commandVelocity(0, 0, 0, YawMode::Zero());
        throw UnsafeMoveException(result);
    }
    //else no unsafe situation

    return true;
}

bool DroneControlBase::safetyCheckVelocity(const Vector3r& velocity)
{
    if (safety_eval_ptr_ == nullptr) //safety checks disabled
        return true;

    const auto& result = safety_eval_ptr_->isSafeVelocity(getPosition(), velocity, getOrientation());
    return emergencyManeuverIfUnsafe(result);
}
bool DroneControlBase::safetyCheckVelocityZ(float vx, float vy, float z)
{
    if (safety_eval_ptr_ == nullptr) //safety checks disabled
        return true;

    const auto& result = safety_eval_ptr_->isSafeVelocityZ(getPosition(), vx, vy, z, getOrientation());
    return emergencyManeuverIfUnsafe(result);
}
bool DroneControlBase::safetyCheckDestination(const Vector3r& dest_pos)
{
    if (safety_eval_ptr_ == nullptr) //safety checks disabled
        return true;

    const auto& result = safety_eval_ptr_->isSafeDestination(getPosition(), dest_pos, getOrientation());
    return emergencyManeuverIfUnsafe(result);
}    

void DroneControlBase::logHomePoint()
{
    GeoPoint homepoint = getHomePoint();
    if (std::isnan(homepoint.longitude))
        Utils::logError("Home point is not set!");
    else
        Utils::logMessage(homepoint.to_string().c_str());
}

float DroneControlBase::setNextPathPosition(const vector<Vector3r>& path, const vector<PathSegment>& path_segs,
    const PathPosition& cur_path_loc, float next_dist, PathPosition& next_path_loc)
{
    //note: cur_path_loc and next_path_loc may both point to same object
    uint i = cur_path_loc.seg_index;
    float offset = cur_path_loc.offset;
    while (i < path.size() - 1) {
        const PathSegment& seg = path_segs.at(i);
        if (seg.seg_length > 0 && //protect against duplicate points in path, normalized seg will have NaN
            seg.seg_length >= next_dist + offset) {

            next_path_loc.seg_index = i;
            next_path_loc.offset = next_dist + offset;  //how much total distance we will travel on this segment
            next_path_loc.position = path.at(i) + seg.seg_normalized * next_path_loc.offset;
            return 0;
        }
        //otherwise use up this segment, move on to next one
        next_dist -= seg.seg_length - offset;
        offset = 0;

        if (&cur_path_loc == &next_path_loc)
            Utils::logMessage("segment %d done: x=%f, y=%f, z=%f", i, path.at(i).x(), path.at(i).z(), path.at(i).z());

        ++i;
    }

    //if we are here then we ran out of segments
    //consider last segment as zero length segment
    next_path_loc.seg_index = i;
    next_path_loc.offset = 0;
    next_path_loc.position = path.at(i);
    return next_dist;
}

void DroneControlBase::adjustYaw(const Vector3r& heading, DrivetrainType drivetrain, YawMode& yaw_mode)
{
    //adjust yaw for the direction of travel in foward-only mode
    if (drivetrain == DrivetrainType::ForwardOnly && !yaw_mode.is_rate) {
        if (heading.norm() > getDistanceAccuracy()) {
            yaw_mode.yaw_or_rate = yaw_mode.yaw_or_rate + (std::atan2(heading.y(), heading.x()) * 180 / M_PIf);
            yaw_mode.yaw_or_rate = VectorMath::normalizeAngleDegrees(yaw_mode.yaw_or_rate);
        }
        else
            yaw_mode.setZeroRate(); //don't change existing yaw if heading is too small because that can generate random result
    }
    //else no adjustment needed
}

void DroneControlBase::adjustYaw(float x, float y, DrivetrainType drivetrain, YawMode& yaw_mode) {
    adjustYaw(Vector3r(x, y, 0), drivetrain, yaw_mode);
}

void DroneControlBase::moveToPathPosition(const Vector3r& dest, float velocity, DrivetrainType drivetrain, /* pass by value */ YawMode yaw_mode, float last_z)
{
    //validate dest
    if (dest.hasNaN())
        throw std::invalid_argument(VectorMath::toString(dest,"dest vector cannot have NaN: "));

    //what is the distance we will travel at this velocity?
    float expected_dist = velocity * getCommandPeriod();

    //get velocity vector
    const Vector3r cur = getPosition();
    const Vector3r cur_dest = dest - cur;
    float cur_dest_norm = cur_dest.norm();

    //yaw for the direction of travel
    adjustYaw(cur_dest, drivetrain, yaw_mode);

    //find velocity vector
    Vector3r velocity_vect;
    if (cur_dest_norm < getDistanceAccuracy())  //our dest is approximately same as current
        velocity_vect = Vector3r::Zero();
    else if (cur_dest_norm >= expected_dist) {
        velocity_vect = (cur_dest / cur_dest_norm) * velocity;
        //Utils::logMessage("velocity_vect=%s", VectorMath::toString(velocity_vect).c_str());
    }
    else { //cur dest is too close than the distance we would travel
            //generate velocity vector that is same size as cur_dest_norm / command period
            //this velocity vect when executed for command period would yield cur_dest_norm
        Utils::logMessage("Too close dest: cur_dest_norm=%f, expected_dist=%f", cur_dest_norm, expected_dist);
        velocity_vect = (cur_dest / cur_dest_norm) * (cur_dest_norm / getCommandPeriod());   
    }

    //send commands
    //try to maintain altitude if path was in XY plan only, velocity based control is not as good
    if (std::abs(cur.z() - dest.z()) <= getDistanceAccuracy()) //for paths in XY plan current code leaves z untouched, so we can compare with strict equality
        moveByVelocityZ(velocity_vect.x(), velocity_vect.y(), dest.z(), yaw_mode);
    else
        moveByVelocity(velocity_vect.x(), velocity_vect.y(), velocity_vect.z(), yaw_mode);
}

bool DroneControlBase::isYawWithinMargin(float yaw_target, float margin)
{
    const float yaw_current = VectorMath::getYaw(getOrientation()) * 180 / M_PIf;
    return std::abs(yaw_current - yaw_target) <= margin;
}    

bool DroneControlBase::setImageTypeForCamera(int camera_id, ImageType type)
{
    if (type == ImageType::None)
        enabled_images.erase(camera_id);
    else
        enabled_images[camera_id] = type;

    return true;
}

DroneControlBase::ImageType DroneControlBase::getImageTypeForCamera(int camera_id)
{
    auto it = enabled_images.find(camera_id);
    if (it != enabled_images.end())
        return it->second;
    return ImageType::None;
}

bool DroneControlBase::setImageForCamera(int camera_id, ImageType type, const vector<uint8_t>& image)
{
    //TODO: perf work
    auto it = images.find(camera_id);
    if (it != images.end())
        (it->second)[type] = image;
    
    auto new_list = std::unordered_map<ImageType, vector<uint8_t>>();
    new_list[type] = image;
    images[camera_id] = new_list;

    return true;
}

vector<uint8_t> DroneControlBase::getImageForCamera(int camera_id, ImageType type)
{
    //TODO: bug: MSGPACK bombs out if vector if of 0 size
    static vector<uint8_t> empty_vec(1);

    //TODO: perf work
    auto it = images.find(camera_id);
    if (it != images.end()) {
        auto it2 = it->second.find(type);
        if (it2 != it->second.end())
            return it2->second;
        else
            return empty_vec;

    } else
        return empty_vec;
}


}} //namespace
#endif
