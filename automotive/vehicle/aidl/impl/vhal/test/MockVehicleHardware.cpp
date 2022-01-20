/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MockVehicleHardware.h"
#include "MockVehicleCallback.h"

#include <utils/Log.h>

namespace android {
namespace hardware {
namespace automotive {
namespace vehicle {

using ::aidl::android::hardware::automotive::vehicle::GetValueRequest;
using ::aidl::android::hardware::automotive::vehicle::GetValueResult;
using ::aidl::android::hardware::automotive::vehicle::SetValueRequest;
using ::aidl::android::hardware::automotive::vehicle::SetValueResult;
using ::aidl::android::hardware::automotive::vehicle::StatusCode;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropConfig;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropValue;

MockVehicleHardware::~MockVehicleHardware() {
    std::unique_lock<std::mutex> lk(mLock);
    mCv.wait(lk, [this] { return mThreadCount == 0; });
}

std::vector<VehiclePropConfig> MockVehicleHardware::getAllPropertyConfigs() const {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    return mPropertyConfigs;
}

StatusCode MockVehicleHardware::setValues(std::shared_ptr<const SetValuesCallback> callback,
                                          const std::vector<SetValueRequest>& requests) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    if (StatusCode status = handleRequestsLocked(__func__, callback, requests, &mSetValueRequests,
                                                 &mSetValueResponses);
        status != StatusCode::OK) {
        return status;
    }
    if (mPropertyChangeCallback == nullptr) {
        return StatusCode::OK;
    }
    std::vector<VehiclePropValue> values;
    for (auto& request : requests) {
        values.push_back(request.value);
    }
    (*mPropertyChangeCallback)(values);
    return StatusCode::OK;
}

StatusCode MockVehicleHardware::getValues(std::shared_ptr<const GetValuesCallback> callback,
                                          const std::vector<GetValueRequest>& requests) const {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    return handleRequestsLocked(__func__, callback, requests, &mGetValueRequests,
                                &mGetValueResponses);
}

DumpResult MockVehicleHardware::dump(const std::vector<std::string>&) {
    // TODO(b/200737967): mock this.
    return DumpResult{};
}

StatusCode MockVehicleHardware::checkHealth() {
    // TODO(b/200737967): mock this.
    return StatusCode::OK;
}

void MockVehicleHardware::registerOnPropertyChangeEvent(
        std::unique_ptr<const PropertyChangeCallback> callback) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    mPropertyChangeCallback = std::move(callback);
}

void MockVehicleHardware::registerOnPropertySetErrorEvent(
        std::unique_ptr<const PropertySetErrorCallback>) {
    // TODO(b/200737967): mock this.
}

void MockVehicleHardware::setPropertyConfigs(const std::vector<VehiclePropConfig>& configs) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    mPropertyConfigs = configs;
}

void MockVehicleHardware::addGetValueResponses(const std::vector<GetValueResult>& responses) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    mGetValueResponses.push_back(responses);
}

void MockVehicleHardware::addSetValueResponses(const std::vector<SetValueResult>& responses) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    mSetValueResponses.push_back(responses);
}

std::vector<GetValueRequest> MockVehicleHardware::nextGetValueRequests() {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    std::optional<std::vector<GetValueRequest>> request = pop(mGetValueRequests);
    if (!request.has_value()) {
        return std::vector<GetValueRequest>();
    }
    return std::move(request.value());
}

std::vector<SetValueRequest> MockVehicleHardware::nextSetValueRequests() {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    std::optional<std::vector<SetValueRequest>> request = pop(mSetValueRequests);
    if (!request.has_value()) {
        return std::vector<SetValueRequest>();
    }
    return std::move(request.value());
}

void MockVehicleHardware::setStatus(const char* functionName, StatusCode status) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    mStatusByFunctions[functionName] = status;
}

void MockVehicleHardware::setSleepTime(int64_t timeInNano) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    mSleepTime = timeInNano;
}

template <class ResultType>
StatusCode MockVehicleHardware::returnResponse(
        std::shared_ptr<const std::function<void(std::vector<ResultType>)>> callback,
        std::list<std::vector<ResultType>>* storedResponses) const {
    if (storedResponses->size() > 0) {
        (*callback)(std::move(storedResponses->front()));
        storedResponses->pop_front();
        return StatusCode::OK;
    } else {
        ALOGE("no more response");
        return StatusCode::INTERNAL_ERROR;
    }
}

template StatusCode MockVehicleHardware::returnResponse<GetValueResult>(
        std::shared_ptr<const std::function<void(std::vector<GetValueResult>)>> callback,
        std::list<std::vector<GetValueResult>>* storedResponses) const;

template StatusCode MockVehicleHardware::returnResponse<SetValueResult>(
        std::shared_ptr<const std::function<void(std::vector<SetValueResult>)>> callback,
        std::list<std::vector<SetValueResult>>* storedResponses) const;

template <class RequestType, class ResultType>
StatusCode MockVehicleHardware::handleRequestsLocked(
        const char* functionName,
        std::shared_ptr<const std::function<void(std::vector<ResultType>)>> callback,
        const std::vector<RequestType>& requests,
        std::list<std::vector<RequestType>>* storedRequests,
        std::list<std::vector<ResultType>>* storedResponses) const {
    storedRequests->push_back(requests);
    if (auto it = mStatusByFunctions.find(functionName); it != mStatusByFunctions.end()) {
        if (StatusCode status = it->second; status != StatusCode::OK) {
            return status;
        }
    }

    if (mSleepTime != 0) {
        int64_t sleepTime = mSleepTime;
        mThreadCount++;
        std::thread t([this, callback, sleepTime, storedResponses]() {
            std::this_thread::sleep_for(std::chrono::nanoseconds(sleepTime));
            returnResponse(callback, storedResponses);
            mThreadCount--;
            mCv.notify_one();
        });
        // Detach the thread here so we do not have to maintain the thread object. mThreadCount
        // and mCv make sure we wait for all threads to end before we exit.
        t.detach();
        return StatusCode::OK;

    } else {
        return returnResponse(callback, storedResponses);
    }
}

template StatusCode MockVehicleHardware::handleRequestsLocked<GetValueRequest, GetValueResult>(
        const char* functionName,
        std::shared_ptr<const std::function<void(std::vector<GetValueResult>)>> callback,
        const std::vector<GetValueRequest>& requests,
        std::list<std::vector<GetValueRequest>>* storedRequests,
        std::list<std::vector<GetValueResult>>* storedResponses) const;

template StatusCode MockVehicleHardware::handleRequestsLocked<SetValueRequest, SetValueResult>(
        const char* functionName,
        std::shared_ptr<const std::function<void(std::vector<SetValueResult>)>> callback,
        const std::vector<SetValueRequest>& requests,
        std::list<std::vector<SetValueRequest>>* storedRequests,
        std::list<std::vector<SetValueResult>>* storedResponses) const;

}  // namespace vehicle
}  // namespace automotive
}  // namespace hardware
}  // namespace android