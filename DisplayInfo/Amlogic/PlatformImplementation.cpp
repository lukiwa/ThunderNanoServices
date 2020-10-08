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
#include "UdevDefinitions.h"
#include "amlDrmUtils.h"
#include <interfaces/IDRM.h>
#include <interfaces/IDisplayInfo.h>
#define AML_HDRSTANDARD_DolbyVision 4
#define AML_HDCP_VERSION_1X 0
#define AML_HDCP_VERSION_2X 1

#define AML_TOTAL_MEM_PARAM_STR "CmaTotal:"
#define AML_FREE_MEM_PARAM_STR "CmaFree:"

#ifdef AMLOGIC_E2
static pthread_mutex_t drmFD_lock = PTHREAD_MUTEX_INITIALIZER;
drmModeConnector* hdmiConn;
drmModeRes* res;

int openDefaultDRMDevice()
{
    int drmFD = -1;
    pthread_mutex_lock(&drmFD_lock);
    if (drmFD < 0) {
        drmFD = open(DEFUALT_DRM_DEVICE, O_RDWR | O_CLOEXEC);
        if (drmFD < 0) {
            printf("%s:%d cannot open %s\n", __FUNCTION__, __LINE__, DEFUALT_DRM_DEVICE);
        }
    }
    pthread_mutex_unlock(&drmFD_lock);
    return drmFD;
}

int getSupportedDRMResolutions(drmModeConnector* conn, drmConnectorModes* drmResolution)
{
    for (int i = 0; i < conn->count_modes; i++) {
        if (!strcmp(conn->modes[i].name, "720x480i")) {
            drmResolution[i] = drmMode_480i;
        } else if (!strcmp(conn->modes[i].name, "720x480")) {
            drmResolution[i] = drmMode_480p;
        } else if (!strcmp(conn->modes[i].name, "1280x720")) {
            drmResolution[i] = drmMode_720p;
        } else if (!strcmp(conn->modes[i].name, "1920x1080i")) {
            drmResolution[i] = drmMode_1080i;
        } else if (!strcmp(conn->modes[i].name, "1920x1080")) {
            if (conn->modes[i].vrefresh == 60) {
                drmResolution[i] = drmMode_1080p;
            } else if (conn->modes[i].vrefresh == 24) {
                drmResolution[i] = drmMode_1080p24;
            } else if (conn->modes[i].vrefresh == 25) {
                drmResolution[i] = drmMode_1080p25;
            } else if (conn->modes[i].vrefresh == 30) {
                drmResolution[i] = drmMode_1080p30;
            } else if (conn->modes[i].vrefresh == 50) {
                drmResolution[i] = drmMode_1080p50;
            } else {
                drmResolution[i] = drmMode_Unknown;
            }
        } else if (!strcmp(conn->modes[i].name, "3840x2160")) {
            if (conn->modes[i].vrefresh == 24) {
                drmResolution[i] = drmMode_3840x2160p24;
            } else if (conn->modes[i].vrefresh == 25) {
                drmResolution[i] = drmMode_3840x2160p25;
            } else if (conn->modes[i].vrefresh == 30) {
                drmResolution[i] = drmMode_3840x2160p30;
            } else if (conn->modes[i].vrefresh == 50) {
                drmResolution[i] = drmMode_3840x2160p50;
            } else if (conn->modes[i].vrefresh == 60) {
                drmResolution[i] = drmMode_3840x2160p60;
            } else {
                drmResolution[i] = drmMode_Unknown;
            }
        } else if (!strcmp(conn->modes[i].name, "4096x2160")) {
            if (conn->modes[i].vrefresh == 24) {
                drmResolution[i] = drmMode_4096x2160p24;
            } else if (conn->modes[i].vrefresh == 25) {
                drmResolution[i] = drmMode_4096x2160p25;
            } else if (conn->modes[i].vrefresh == 30) {
                drmResolution[i] = drmMode_4096x2160p30;
            } else if (conn->modes[i].vrefresh == 50) {
                drmResolution[i] = drmMode_4096x2160p50;
            } else if (conn->modes[i].vrefresh == 60) {
                drmResolution[i] = drmMode_4096x2160p60;
            } else {
                drmResolution[i] = drmMode_Unknown;
            }
        }
    }
    return 0;
}
int amsysfs_get_sysfs_str(const char* path, char* valstr, int size)
{
    int fd;
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        memset(valstr, 0, size);
        read(fd, valstr, size - 1);
        valstr[strlen(valstr)] = '\0';
        close(fd);
    } else {
        printf("%s:%d unable to open file %s,err: %s\n", __FUNCTION__, __LINE__, path, strerror(errno));
        sprintf(valstr, "%s", "fail");
        return -1;
    };
    return 0;
}

#endif

namespace WPEFramework {
namespace Plugin {

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

                _parent.UpdateDisplayProperties();

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
            , _width(0)
            , _height(0)
            , _connected(false)
            , _verticalFreq(0)
            , _hdcpprotection(HDCPProtectionType::HDCP_Unencrypted)
            , _type(HDR_OFF)
            , _freeGpuRam(0)
            , _totalGpuRam(0)
            , _audioPassthrough(false)
            , _adminLock()
            , _activity(*this)
        {
            UpdateDisplayProperties();
        }

        DisplayInfoImplementation(const DisplayInfoImplementation&) = delete;
        DisplayInfoImplementation& operator=(const DisplayInfoImplementation&) = delete;
        ~DisplayInfoImplementation() override = default;

    public:
        uint32_t TotalGpuRam(uint64_t& total) const override
        {
            // TODO: Fix this
            total = GetMemory(AML_TOTAL_MEM_PARAM_STR);
            return Core::ERROR_NONE;
        }

        uint32_t FreeGpuRam(uint64_t& free) const override
        {
            // TODO: Fix this
            free = GetMemory(AML_FREE_MEM_PARAM_STR);
            return Core::ERROR_NONE;
        }

        uint32_t Register(INotification* notification) override
        {
            _adminLock.Lock();

            // Make sure a sink is not registered multiple times.
            auto index = std::find(_observers.begin(), _observers.end(), notification);
            ASSERT(index == _observers.end());

            if (index == _observers.end()) {
                _observers.push_back(notification);
                notification->AddRef();
            }

            _adminLock.Unlock();

            return (Core::ERROR_NONE);
        }

        uint32_t Unregister(INotification* notification) override
        {
            _adminLock.Lock();

            std::list<IConnectionProperties::INotification*>::iterator index(std::find(_observers.begin(), _observers.end(), notification));

            // Make sure you do not unregister something you did not register !!!
            ASSERT(index != _observers.end());

            if (index != _observers.end()) {
                (*index)->Release();
                _observers.erase(index);
            }

            _adminLock.Unlock();

            return (Core::ERROR_NONE);
        }

        uint32_t IsAudioPassthrough(bool& passthru) const override
        {
            passthru = _audioPassthrough;
            return Core::ERROR_NONE;
        }

        uint32_t Connected(bool& isconnected) const override
        {
            isconnected = _connected;
            return Core::ERROR_NONE;
        }

        uint32_t Width(uint32_t& width) const override
        {
            width = _width;
            return Core::ERROR_NONE;
        }

        uint32_t Height(uint32_t& height) const override
        {
            height = _height;
            return Core::ERROR_NONE;
        }

        uint32_t VerticalFreq(uint32_t& vf) const override
        {
            vf = _verticalFreq;
            return Core::ERROR_NONE;
        }

        uint32_t EDID(uint16_t& length, uint8_t data[]) const override
        {
            return Core::ERROR_UNAVAILABLE;
        }

        uint32_t HDCPProtection(HDCPProtectionType& value) const override
        {
            value = _hdcpprotection;
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
            type = _type;
            return Core::ERROR_NONE;
        }

        void Dispatch() const
        {
            _adminLock.Lock();

            std::list<IConnectionProperties::INotification*>::const_iterator index = _observers.begin();

            if (index != _observers.end()) {
                // Placeholder INotification::Source value.
                (*index)->Updated(IConnectionProperties::INotification::Source::HDCP_CHANGE);
            }

            _adminLock.Unlock();
        }

        BEGIN_INTERFACE_MAP(DisplayInfoImplementation)
        INTERFACE_ENTRY(Exchange::IGraphicsProperties)
        INTERFACE_ENTRY(Exchange::IConnectionProperties)
        INTERFACE_ENTRY(Exchange::IHDRProperties)
        END_INTERFACE_MAP

    private:
        uint64_t parseLine(const char* line)
        {
            string str(line);
            uint64_t val = 0;
            size_t begin = str.find_first_of("0123456789");
            size_t end = std::string::npos;

            if (std::string::npos != begin)
                end = str.find_first_not_of("0123456789", begin);

            if (std::string::npos != begin && std::string::npos != end) {

                str = str.substr(begin, end);
                val = strtoul(str.c_str(), NULL, 10);

            } else {
                printf("%s:%d Failed to parse value from %s", __FUNCTION__, __LINE__, line);
            }

            return val;
        }

        uint64_t GetGPUMemory(const char* param)
        {

            uint64_t memVal = 0;
            FILE* meminfoFile = fopen("/proc/meminfo", "r");
            if (NULL == meminfoFile) {
                printf("%s:%d : Failed to open /proc/meminfo:%s", __FUNCTION__, __LINE__, strerror(errno));
            } else {
                std::vector<char> buf;
                buf.resize(1024);

                while (fgets(buf.data(), buf.size(), meminfoFile)) {
                    if (strstr(buf.data(), param) == buf.data()) {
                        memVal = parseLine(buf.data()) * 1000;
                        break;
                    }
                }

                fclose(meminfoFile);
            }
            return memVal;
        }

        void UpdateDisplay()
        {
#ifdef AMLOGIC_E2
            char strStatus[13] = { '\0' };

            amsysfs_get_sysfs_str("/sys/class/drm/card0-HDMI-A-1/status", strStatus, sizeof(strStatus));
            if (strncmp(strStatus, "connected", 9) == 0) {
                _connected = true;
            } else {
                _connected = false;
            }

            amlError_t ret = amlERR_NONE;
            bool drmInitialized = false;
            int drmFD = -1;
            if (!drmInitialized) {
                bool acquiredConnector = false;
                drmFD = openDefaultDRMDevice();
                if (drmFD < 0) {
                    ret = amlERR_GENERAL;
                }
                /* retrieve resources */
                res = drmModeGetResources(drmFD);
                if (!res) {
                    fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
                        errno);
                    ret = amlERR_GENERAL;
                }

                while (!acquiredConnector) {
                    for (int i = 0; i < res->count_connectors; ++i) {
                        /* get information for each connector */
                        hdmiConn = drmModeGetConnector(drmFD, res->connectors[i]);
                        if (!hdmiConn) {
                            fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
                                i, res->connectors[i], errno);
                            continue;
                        }
                        if (hdmiConn->connector_type == DRM_MODE_CONNECTOR_HDMIA) { //Save connector pointer for HDMI Tx
                            acquiredConnector = true;
                            break;
                        }

                        continue;
                    }
                }
            }
            drmConnectorModes supportedModes[drmMode_Max] = { drmMode_Unknown };
            getSupportedDRMResolutions(hdmiConn, supportedModes);
            for (int i = 0; i < drmMode_Max; i++) {
                switch (supportedModes[i]) {
                case drmMode_3840x2160p24:
                case drmMode_3840x2160p25:
                case drmMode_3840x2160p30:
                case drmMode_3840x2160p50:
                case drmMode_4096x2160p24:
                case drmMode_4096x2160p25:
                case drmMode_4096x2160p30:
                case drmMode_4096x2160p50:
                    height = 2160;
                    width = 4096;
                    break;
                case drmMode_3840x2160p60:
                case drmMode_4096x2160p60:
                    height = 2160;
                    width = 4096;
                    break;
                default:
                    break;
                }
            }
#else
            _connected = true;
            _verticalFreq = 60;
            _height = 2160;
            _width = 4096;
#endif
            _type = HDR_DOLBYVISION;
        }

        void UpdateDisplayProperties()
        {
            _propertiesLock.Lock();

            UpdateDisplay();

            _totalGpuRam = GetGPUMemory(AML_TOTAL_MEM_PARAM_STR);
            _freeGpuRam = GetGPUMemory(AML_FREE_MEM_PARAM_STR);

            _propertiesLock.Unlock();

            _activity.Submit();
        }

    private:
        ConnectionObserver _hdmiObserver;
        uint32_t _width;
        uint32_t _height;
        bool _connected;
        uint32_t _verticalFreq;
        HDCPProtectionType _hdcpprotection;
        HDRType _type;
        uint64_t _freeGpuRam;
        uint64_t _totalGpuRam;
        bool _audioPassthrough;
        mutable Core::CriticalSection _propertiesLock;

        std::list<IConnectionProperties::INotification*> _observers;
        mutable Core::CriticalSection _observersLock;

        Core::WorkerPool::JobType<DisplayInfoImplementation&> _activity;
    };

    SERVICE_REGISTRATION(DisplayInfoImplementation, 1, 0);
}
}
