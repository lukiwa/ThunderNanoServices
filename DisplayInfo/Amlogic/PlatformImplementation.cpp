/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../Module.h"
#include "DRMConnector.h"
#include <core/Portability.h>
#include <interfaces/IDRM.h>
#include <interfaces/IDisplayInfo.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <fstream>
#include <string>

namespace WPEFramework {
namespace Plugin {

    namespace {
        /** 
         * The following are structures copied from udev in order to properly parse messages coming in through
         * the socket connection(ConnectionObserver class).
         * Found @ https://github.com/systemd/systemd/blob/master/src/libsystemd/sd-device/device-monitor.c#L50
         */
#define UDEV_MONITOR_MAGIC 0xfeedcafe
        struct udev_monitor_netlink_header {
            /* "libudev" prefix to distinguish libudev and kernel messages */
            char prefix[8];
            /*
            * magic to protect against daemon <-> library message format mismatch
            * used in the kernel from socket filter rules; needs to be stored in network order
            */
            unsigned int magic;
            /* total length of header structure known to the sender */
            unsigned int header_size;
            /* properties string buffer */
            unsigned int properties_off;
            unsigned int properties_len;
            /*
            * hashes of primary device properties strings, to let libudev subscribers
            * use in-kernel socket filters; values need to be stored in network order
            */
            unsigned int filter_subsystem_hash;
            unsigned int filter_devtype_hash;
            unsigned int filter_tag_bloom_hi;
            unsigned int filter_tag_bloom_lo;
        };
    }

    class DisplayInfoImplementation
        : public Exchange::IGraphicsProperties,
          public Exchange::IConnectionProperties,
          public Exchange::IHDRProperties {

    private:
        class ConnectionObserver
            : public Core::SocketDatagram,
              public Core::Thread {

        public:
            ConnectionObserver(const ConnectionObserver&) = delete;
            const ConnectionObserver& operator=(const ConnectionObserver&) = delete;

            /**
             * @brief Creates a NETLINK socket connection to get notified about udev messages
             * related to HDMI hotplugs.
             */
            ConnectionObserver(DisplayInfoImplementation& parent)
                // The group value of "2" stands for GROUP_UDEV, which filters out kernel messages,
                // occuring before the device is initialized.
                // https://insujang.github.io/2018-11-27/udev-device-manager-for-the-linux-kernel-in-userspace/
                : Core::SocketDatagram(false, Core::NodeId(NETLINK_KOBJECT_UEVENT, 0, 2), Core::NodeId(), 512, 1024)
                , _parent(parent)
                , _requeryProps(false, true)
            {
                Open(Core::infinite);
                Run();
            }

            ~ConnectionObserver() override
            {
                Stop();
                _requeryProps.SetEvent();
                Close(Core::infinite);
            }

            void StateChange() override {}

            uint16_t SendData(uint8_t* dataFrame, const uint16_t maxSendSize) override
            {
                return 0;
            }

            uint16_t ReceiveData(uint8_t* dataFrame, const uint16_t receivedSize) override
            {
                bool drmEvent = false;
                bool hotPlugEvent = false;

                udev_monitor_netlink_header* header = reinterpret_cast<udev_monitor_netlink_header*>(dataFrame);

                /**
                 * TODO: These tags signify some filter, for now just filter out the
                 * message that has this value as 0. Investigate further within udev,
                 * what do these filters mean and how to use them in order to
                 * get only a single message per hotplug.
                 */
                if (header->filter_tag_bloom_hi != 0 && header->filter_tag_bloom_lo != 0) {

                    int data_index = header->properties_off / sizeof(uint8_t);
                    auto data_ptr = reinterpret_cast<char*>(&(dataFrame[data_index]));
                    auto values = GetMessageValues(data_ptr, header->properties_len);

                    for (auto& value : values) {
                        if (value == "DEVTYPE=drm_minor") {
                            drmEvent = true;
                        } else if (value == "HOTPLUG=1") {
                            hotPlugEvent = true;
                        }
                    }
                }

                if (drmEvent && hotPlugEvent) {
                    _requeryProps.SetEvent();
                }

                return receivedSize;
            }

            uint32_t Worker() override
            {
                _requeryProps.Lock();

                _parent.Reinitialize();

                _requeryProps.ResetEvent();

                return (Core::infinite);
            }

        private:
            uint16_t Events() override
            {
                return IsOpen() ? POLLIN : 0;
            }

            std::vector<std::string> GetMessageValues(char* values, uint32_t size)
            {
                std::vector<std::string> output;
                for (int i = 0, output_index = 0; i < size; ++output_index) {
                    char* data = &values[i];
                    output.push_back(std::string(data));
                    i += (strlen(data) + 1);
                }
                return output;
            }

            DisplayInfoImplementation& _parent;
            Core::Event _requeryProps;
        };

    public:
        DisplayInfoImplementation()
            : _hdmiObserver(*this)
            , _usePreferredDrmMode(false)
            , _drmCard(DEFAULT_DRM_DEVICE)
            , _width(0)
            , _height(0)
            , _connected(false)
            , _verticalFreq(0)
            , _hdcpprotection(HDCPProtectionType::HDCP_Unencrypted)
            , _hdrType(HDR_OFF)
            , _freeGpuRam(0)
            , _totalGpuRam(0)
            , _audioPassthrough(false)
            , _edid()
            , _propertiesLock()
            , _observersLock()
            , _activity(*this)
        {
            _usePreferredDrmMode = (std::getenv("WESTEROS_GL_USE_PREFERRED_MODE") != nullptr);
            _useBestDrmMode = (std::getenv("WESTEROS_GL_USE_BEST_MODE") != nullptr);

            std::string tmpCardName;
            if (Core::SystemInfo::GetEnvironment("WESTEROS_DRM_CARD", tmpCardName) && !tmpCardName.empty()) {
                _drmCard = tmpCardName;
            }
            Reinitialize();
        }

        DisplayInfoImplementation(const DisplayInfoImplementation&) = delete;
        DisplayInfoImplementation& operator=(const DisplayInfoImplementation&) = delete;
        ~DisplayInfoImplementation() override = default;

    public:
        uint32_t TotalGpuRam(uint64_t& total) const override
        {
            _propertiesLock.Lock();
            total = _totalGpuRam;
            _propertiesLock.Unlock();

            return Core::ERROR_NONE;
        }

        uint32_t FreeGpuRam(uint64_t& free) const override
        {
            _propertiesLock.Lock();
            free = _freeGpuRam;
            _propertiesLock.Unlock();

            return Core::ERROR_NONE;
        }

        uint32_t Register(INotification* notification) override
        {
            _observersLock.Lock();

            auto index = std::find(_observers.begin(), _observers.end(), notification);
            ASSERT(index == _observers.end());

            if (index == _observers.end()) {
                _observers.push_back(notification);
                notification->AddRef();
            }

            _observersLock.Unlock();

            return (Core::ERROR_NONE);
        }

        uint32_t Unregister(INotification* notification) override
        {
            _observersLock.Lock();

            std::list<IConnectionProperties::INotification*>::iterator index(std::find(_observers.begin(), _observers.end(), notification));

            ASSERT(index != _observers.end());

            if (index != _observers.end()) {
                (*index)->Release();
                _observers.erase(index);
            }

            _observersLock.Unlock();

            return (Core::ERROR_NONE);
        }

        uint32_t IsAudioPassthrough(bool& passthru) const override
        {
            return Core::ERROR_UNAVAILABLE;
        }

        uint32_t Connected(bool& isconnected) const override
        {
            _propertiesLock.Lock();
            isconnected = _connected;
            _propertiesLock.Unlock();

            return Core::ERROR_NONE;
        }

        uint32_t Width(uint32_t& width) const override
        {
            _propertiesLock.Lock();
            width = _width;
            _propertiesLock.Unlock();

            return Core::ERROR_NONE;
        }

        uint32_t Height(uint32_t& height) const override
        {
            _propertiesLock.Lock();
            height = _height;
            _propertiesLock.Unlock();

            return Core::ERROR_NONE;
        }

        uint32_t VerticalFreq(uint32_t& vf) const override
        {
            _propertiesLock.Lock();
            vf = _verticalFreq;
            _propertiesLock.Unlock();

            return Core::ERROR_NONE;
        }

        uint32_t EDID(uint16_t& length, uint8_t data[]) const override
        {
            _propertiesLock.Lock();
            data = const_cast<uint8_t*>(_edid.data());
            length = _edid.size();
            _propertiesLock.Unlock();

            return Core::ERROR_NONE;
        }

        uint32_t WidthInCentimeters(uint8_t& width /* @out */) const override
        {
            return Core::ERROR_UNAVAILABLE;
        }

        uint32_t HeightInCentimeters(uint8_t& heigth /* @out */) const override
        {
            return Core::ERROR_UNAVAILABLE;
        }

        uint32_t HDCPProtection(HDCPProtectionType& value) const override
        {
            _propertiesLock.Lock();
            value = _hdcpprotection;
            _propertiesLock.Unlock();

            return Core::ERROR_NONE;
        }

        uint32_t HDCPProtection(const HDCPProtectionType value) override
        {
            return Core::ERROR_UNAVAILABLE;
        }

        uint32_t PortName(string& name) const override
        {
            return Core::ERROR_UNAVAILABLE;
        }

        uint32_t TVCapabilities(IHDRIterator*& type) const override
        {
            return Core::ERROR_UNAVAILABLE;
        }

        uint32_t STBCapabilities(IHDRIterator*& type) const override
        {
            return Core::ERROR_UNAVAILABLE;
        }

        uint32_t HDRSetting(HDRType& type) const override
        {
            _propertiesLock.Lock();
            type = _hdrType;
            _propertiesLock.Unlock();

            return Core::ERROR_NONE;
        }

        void Dispatch() const
        {
            _observersLock.Lock();

            std::list<IConnectionProperties::INotification*>::const_iterator index = _observers.begin();

            if (index != _observers.end()) {
                (*index)->Updated(IConnectionProperties::INotification::Source::HDMI_CHANGE);
            }

            _observersLock.Unlock();
        }

        BEGIN_INTERFACE_MAP(DisplayInfoImplementation)
        INTERFACE_ENTRY(Exchange::IGraphicsProperties)
        INTERFACE_ENTRY(Exchange::IConnectionProperties)
        INTERFACE_ENTRY(Exchange::IHDRProperties)
        END_INTERFACE_MAP

    private:
        static constexpr auto DEFAULT_DRM_DEVICE = "/dev/dri/card0";
        static constexpr auto HDMI_STATUS_NODE = "/sys/devices/platform/drm-subsystem/drm/card0/card0-HDMI-A-1/status";
        static constexpr auto EDID_NODE = "/sys/devices/platform/drm-subsystem/drm/card0/card0-HDMI-A-1/edid";
        static constexpr auto HDR_LEVEL_NODE = "/sys/devices/virtual/amhdmitx/amhdmitx0/hdmi_hdr_status";
        static constexpr auto HDCP_LEVEL_NODE = "/sys/module/hdmitx20/parameters/hdmi_authenticated";
        static constexpr auto TOTAL_GPU_MEM_KEY = "CmaTotal";
        static constexpr auto FREE_GPU_MEM_KEY = "CmaFree";

        bool IsConnected()
        {
            return getLine(HDMI_STATUS_NODE) == "connected";
        }

        std::string getLine(const std::string& filepath)
        {
            std::string line;
            std::ifstream statusFile(filepath);

            if (statusFile.is_open()) {
                getline(statusFile, line);
                statusFile.close();
            } else {
                TRACE(Trace::Error, (_T("Could not open file: %s"), filepath.c_str()));
            }
            return line;
        }

        void Reinitialize()
        {
            _propertiesLock.Lock();

            _connected = IsConnected();

            if (_connected) {
                UpdateDisplayProperties();
                UpdateEDID();
                UpdateProtectionProperties();
                UpdateGraphicsProperties();
                UpdateHDRProperties();
            } else {
                ResetValues();
            }

            /* clang-format off */
            TRACE(Trace::Information, (_T("HDMI: %s, %dx%d %d [Hz], HDCPProtectionType: %d, HDRType: %d")
                , (_connected ? "on" : "off")
                , _width
                , _height
                , _verticalFreq
                , static_cast<int>(_hdcpprotection)
                , static_cast<int>(_hdrType)));
            /* clang-format on */

            _propertiesLock.Unlock();

            _activity.Submit();
        }

        void UpdateDisplayProperties()
        {
            using namespace AMLogic;
            DRMConnector connector(_drmCard, DRM_MODE_CONNECTOR_HDMIA);
            drmModeModeInfoPtr selectedMode = _usePreferredDrmMode ? connector.PreferredMode()
                                                                   : (_useBestDrmMode ? connector.BestMode() : connector.DefaultMode());

            if (selectedMode != nullptr) {
                _width = selectedMode->hdisplay;
                _height = selectedMode->vdisplay;
                _verticalFreq = selectedMode->vrefresh;
            } else {
                _width = 0;
                _height = 0;
                _verticalFreq = 0;
            }
        }

        void UpdateEDID()
        {
            std::ifstream instream(EDID_NODE, std::ios::in | std::ios::binary);
            _edid = std::vector<uint8_t>((std::istreambuf_iterator<char>(instream)),
                std::istreambuf_iterator<char>());
        }

        void UpdateProtectionProperties()
        {
            std::string hdcpStr = getLine(HDCP_LEVEL_NODE);
            if (hdcpStr == "22") {
                _hdcpprotection = Exchange::IConnectionProperties::HDCPProtectionType::HDCP_2X;
            } else if (hdcpStr == "14") {
                _hdcpprotection = Exchange::IConnectionProperties::HDCPProtectionType::HDCP_1X;
            } else {
                TRACE(Trace::Error, (_T("Received HDCP value: %s"), hdcpStr.c_str()));
                _hdcpprotection = Exchange::IConnectionProperties::HDCPProtectionType::HDCP_Unencrypted;
            }
        }

        void UpdateHDRProperties()
        {
            std::string hdrStr = getLine(HDR_LEVEL_NODE);
            if (hdrStr == "SDR") {
                _hdrType = Exchange::IHDRProperties::HDRType::HDR_OFF;
            } else if (hdrStr == "HDR10-GAMMA_ST2084") {
                _hdrType = Exchange::IHDRProperties::HDRType::HDR_10;
            } else if (hdrStr == "HDR10-GAMMA_HLG") {
                _hdrType = Exchange::IHDRProperties::HDRType::HDR_HLG;
            } else if (hdrStr == "HDR10Plus-VSIF") {
                _hdrType = Exchange::IHDRProperties::HDRType::HDR_10PLUS;
            } else if (hdrStr == "DolbyVision-Std" || hdrStr == "DolbyVision-Lowlatency") {
                _hdrType = Exchange::IHDRProperties::HDRType::HDR_DOLBYVISION;
            } else if (hdrStr == "HDR10-others") {
                TRACE(Trace::Warning, (_T("Received unknown HDR10 value. Falling back to HDR10.")));
                _hdrType = Exchange::IHDRProperties::HDRType::HDR_10;
            } else {
                TRACE(Trace::Error, (_T("Received unknown HDR value %s. Falling back to SDR."), hdrStr.c_str()));
            }
        }

        void UpdateGraphicsProperties()
        {
            auto extractNumbers = [](const std::string& str) {
                auto first = str.find_first_of("0123456789");
                auto last = str.find_last_of("0123456789");
                return std::stoul(str.substr(first, last - first + 1));
            };

            auto values = getMemoryStats("/proc/meminfo", { TOTAL_GPU_MEM_KEY, FREE_GPU_MEM_KEY });
            if (!values.first.empty() && !values.second.empty()) {
                _freeGpuRam = extractNumbers(values.second);
                _totalGpuRam = extractNumbers(values.first);
            } else {
                _totalGpuRam = 0;
                _freeGpuRam = 0;
            }
        }

        std::pair<std::string, std::string> getMemoryStats(const std::string& filepath,
            const std::pair<std::string, std::string>& keys)
        {
            std::string line;
            std::ifstream statusFile(filepath);
            std::pair<std::string, std::string> result;

            if (statusFile.is_open()) {
                while ((result.first.empty() || result.second.empty()) && getline(statusFile, line)) {
                    if (line.find(keys.first) != std::string::npos)
                        result.first = line;
                    else if (line.find(keys.second) != std::string::npos)
                        result.second = line;
                }
                statusFile.close();
            } else {
                TRACE(Trace::Error, (_T("Could not open file: %s"), filepath.c_str()));
            }
            return result;
        }

        void ResetValues()
        {
            _height = 0;
            _width = 0;
            _verticalFreq = 0;
            _hdcpprotection = Exchange::IConnectionProperties::HDCPProtectionType::HDCP_Unencrypted;
            _hdrType = Exchange::IHDRProperties::HDRType::HDR_OFF;
            _totalGpuRam = 0;
            _freeGpuRam = 0;
            _audioPassthrough = false;
            _edid.clear();
        }

        bool _usePreferredDrmMode;
        bool _useBestDrmMode;
        std::string _drmCard;

        ConnectionObserver _hdmiObserver;
        uint32_t _width;
        uint32_t _height;
        bool _connected;
        uint32_t _verticalFreq;
        HDCPProtectionType _hdcpprotection;
        HDRType _hdrType;
        uint64_t _totalGpuRam;
        uint64_t _freeGpuRam;
        bool _audioPassthrough;
        std::vector<uint8_t> _edid;
        mutable Core::CriticalSection _propertiesLock;

        Core::WorkerPool::JobType<DisplayInfoImplementation&> _activity;
        std::list<IConnectionProperties::INotification*> _observers;
        mutable Core::CriticalSection _observersLock;
    };

    SERVICE_REGISTRATION(DisplayInfoImplementation, 1, 0);
}
}
