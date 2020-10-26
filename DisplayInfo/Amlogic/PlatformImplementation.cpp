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
#include <core/Portability.h>
#include <interfaces/IDRM.h>
#include <interfaces/IDisplayInfo.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <fstream>

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

namespace WPEFramework {
namespace Plugin {

    namespace {
        // The following are structures copied from udev in order to properly parse messages coming in through
        // the socket connection(ConnectionObserver class).
        // Found @ https://github.com/systemd/systemd/blob/master/src/libsystemd/sd-device/device-monitor.c#L50
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

                // TODO: These tags signify some filter, for now just filter out the 
                // message that has this value as 0. Investigate further within udev,
                // what do these filters mean and how to use them in order to
                // get only a single message per hotplug.
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
            , _propertiesLock()
            , _observersLock()
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

            // Make sure a sink is not registered multiple times.
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

            // Make sure you do not unregister something you did not register !!!
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
            _propertiesLock.Lock();
            passthru = _audioPassthrough;
            _propertiesLock.Unlock();

            return Core::ERROR_NONE;
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
            return Core::ERROR_UNAVAILABLE;
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
            type = _type;
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
            // TODO: The following doesn't query the GPU ram.
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

        uint32_t UpdateDisplay()
        {
            uint32_t result = Core::ERROR_NONE;
            int drmFD = open(DEFUALT_DRM_DEVICE, O_RDWR | O_CLOEXEC);
            if (drmFD < 0) {
                result = Core::ERROR_ILLEGAL_STATE;
            } else if (drmModeRes* res = drmModeGetResources(drmFD)) {

                for (int i = 0; i < res->count_connectors; ++i) {

                    drmModeConnector* hdmiConn = drmModeGetConnector(drmFD, res->connectors[i]);

                    if (hdmiConn && hdmiConn->connector_type == DRM_MODE_CONNECTOR_HDMIA) {
                        // TODO: There are multiple modes, which have the same resolution,
                        // refresh rate, flags and type. Figure out which one should be picked.
                        if (hdmiConn->modes) {
                            _widthInCm = (hdmiConn->mmWidth / 10);
                            _width = static_cast<uint32_t>(hdmiConn->modes[0].hdisplay);

                            _heightInCm = (hdmiConn->mmHeight / 10);
                            _height = static_cast<uint32_t>(hdmiConn->modes[0].vdisplay);

                            _verticalFreq = hdmiConn->modes[0].vrefresh;
                        }
                        drmModeFreeConnector(hdmiConn);
                    }
                }
                drmModeFreeResources(res);
                close(drmFD);
            }
        }

        void UpdateDisplayProperties()
        {
            _propertiesLock.Lock();

            _connected = IsConnected();

            if (_connected) {
                UpdateDisplay();
                _totalGpuRam = GetGPUMemory(AML_TOTAL_MEM_PARAM_STR);
                _freeGpuRam = GetGPUMemory(AML_FREE_MEM_PARAM_STR);
            } else {
                _height = 0;
                _heightInCm = 0;
                _width = 0;
                _widthInCm = 0;
                _verticalFreq = 0;
            }

            _propertiesLock.Unlock();

            _activity.Submit();
        }

    private:
        bool IsConnected()
        {
            std::string line;
            std::ifstream statusFile(STATUS_FILEPATH);
            if (statusFile.is_open()) {
                getline(statusFile, line);
                statusFile.close();
            }
            return line == "connected";
        }

        static constexpr auto STATUS_FILEPATH = "/sys/class/drm/card0-HDMI-A-1/status";
        static constexpr auto AML_TOTAL_MEM_PARAM_STR = "CmaTotal:";
        static constexpr auto AML_FREE_MEM_PARAM_STR = "CmaFree:";
        static constexpr auto DEFUALT_DRM_DEVICE = "/dev/dri/card0";

        ConnectionObserver _hdmiObserver;
        uint32_t _width;
        uint32_t _widthInCm;
        uint32_t _height;
        uint32_t _heightInCm;
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
