/*
**
** Copyright 2022, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

// #define LOG_NDEBUG 0
#define LOG_TAG "AudioFlinger::MelReporter"

#include "AudioFlinger.h"

#include <android/media/ISoundDoseCallback.h>
#include <audio_utils/power.h>
#include <utils/Log.h>

namespace android {

bool AudioFlinger::MelReporter::shouldComputeMelForDeviceType(audio_devices_t device) {
    switch (device) {
        case AUDIO_DEVICE_OUT_WIRED_HEADSET:
        case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
        case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP:
        case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES:
        case AUDIO_DEVICE_OUT_HEARING_AID:
        case AUDIO_DEVICE_OUT_USB_HEADSET:
        case AUDIO_DEVICE_OUT_BLE_HEADSET:
        case AUDIO_DEVICE_OUT_BLE_BROADCAST:
            return true;
        default:
            return false;
    }
}

void AudioFlinger::MelReporter::onCreateAudioPatch(audio_patch_handle_t handle,
        const PatchPanel::Patch& patch) {
    ALOGV("%s: handle %d mHalHandle %d device sink %08x",
            __func__, handle, patch.mHalHandle,
            patch.mAudioPatch.num_sinks > 0 ? patch.mAudioPatch.sinks[0].ext.device.type : 0);
    if (patch.mAudioPatch.num_sources == 0
        || patch.mAudioPatch.sources[0].type != AUDIO_PORT_TYPE_MIX) {
        ALOGW("%s: patch does not contain any mix sources", __func__);
        return;
    }

    audio_io_handle_t streamHandle = patch.mAudioPatch.sources[0].ext.mix.handle;
    ActiveMelPatch newPatch;
    newPatch.streamHandle = streamHandle;
    for (int i = 0; i < patch.mAudioPatch.num_sinks; ++ i) {
        if (patch.mAudioPatch.sinks[i].type == AUDIO_PORT_TYPE_DEVICE
            && shouldComputeMelForDeviceType(patch.mAudioPatch.sinks[i].ext.device.type)) {
            audio_port_handle_t deviceId = patch.mAudioPatch.sinks[i].id;
            newPatch.deviceHandles.push_back(deviceId);

            // Start the MEL calculation in the PlaybackThread
            std::lock_guard _lAf(mAudioFlinger.mLock);
            auto thread = mAudioFlinger.checkPlaybackThread_l(streamHandle);
            if (thread != nullptr) {
                thread->startMelComputation(mSoundDoseManager.getOrCreateProcessorForDevice(
                    deviceId,
                    newPatch.streamHandle,
                    thread->mSampleRate,
                    thread->mChannelCount,
                    thread->mFormat));
            }
        }
    }

    std::lock_guard _l(mLock);
    mActiveMelPatches[patch.mAudioPatch.id] = newPatch;
}

void AudioFlinger::MelReporter::onReleaseAudioPatch(audio_patch_handle_t handle) {
    ALOGV("%s", __func__);

    ActiveMelPatch melPatch;
    {
        std::lock_guard _l(mLock);

        auto patchIt = mActiveMelPatches.find(handle);
        if (patchIt == mActiveMelPatches.end()) {
            ALOGW(
                "%s patch does not contain any mix sources with active MEL calculation",
                __func__);
            return;
        }

        melPatch = patchIt->second;
        mActiveMelPatches.erase(patchIt);
    }

    // Stop MEL calculation for the PlaybackThread
    std::lock_guard _lAf(mAudioFlinger.mLock);
    mSoundDoseManager.removeStreamProcessor(melPatch.streamHandle);
    auto thread = mAudioFlinger.checkPlaybackThread_l(melPatch.streamHandle);
    if (thread != nullptr) {
        thread->stopMelComputation();
    }
}

sp<media::ISoundDose> AudioFlinger::MelReporter::getSoundDoseInterface(
        const sp<media::ISoundDoseCallback>& callback) {
    // no need to lock since getSoundDoseInterface is synchronized
    return mSoundDoseManager.getSoundDoseInterface(callback);
}

std::string AudioFlinger::MelReporter::dump() {
    std::lock_guard _l(mLock);
    std::string output("\nSound Dose:\n");
    output.append(mSoundDoseManager.dump());
    return output;
}

}  // namespace android
