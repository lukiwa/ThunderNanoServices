#pragma once

#include <core/Portability.h>
#include <tracing/Logging.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <string>
#include <vector>

namespace WPEFramework {
namespace Plugin {
    namespace AMLogic {

        bool operator<(const drmModeModeInfo& lhs, const drmModeModeInfo& rhs)
        {
            return (lhs.hdisplay * lhs.vdisplay) < (rhs.hdisplay * rhs.vdisplay);
        }

        bool operator>(const drmModeModeInfo& lhs, const drmModeModeInfo& rhs)
        {
            return !(lhs < rhs);
        }

        /**
         * @brief RAII-style wrapper class used for safe access to drmModeConnector's properties.
         */
        class DRMConnector {
        public:
            DRMConnector(const std::string& drmDevice, int drmConnectorType)
                : _hdmiConnector(nullptr)
                , _drmResources(nullptr)
                ,_drmDevice(drmDevice)
                , _modes(nullptr)
                , _modeCount(0)
            {
                _drmFd = open(_drmDevice.c_str(), O_RDWR | O_CLOEXEC);
                if (_drmFd < 0) {
                    TRACE(Trace::Error, (_T("Could not open DRM device with path: %s"), _drmDevice.c_str()));
                } else if (_drmResources = drmModeGetResources(_drmFd)) {
                    for (int i = 0; i < _drmResources->count_connectors; ++i) {
                        drmModeConnector* connector = drmModeGetConnector(_drmFd, _drmResources->connectors[i]);
                        if (connector && connector->connector_type == drmConnectorType) {
                            _hdmiConnector = connector;
                            _modes = _hdmiConnector->modes;
                            _modeCount = _hdmiConnector->count_modes;
                        } else {
                            drmModeFreeConnector(connector);
                        }
                    }
                }
            }

            /**
             * @brief Picks the connector mode based on highest available 
             * resolution. Most commonly used.
             * @return drmModeModeInfoPtr
             */
            drmModeModeInfoPtr BestMode() const
            {
                drmModeModeInfoPtr result = nullptr;
                for (int index = 0; index < _modeCount; ++index) {
                    if (result == nullptr) {
                        result = _modes + index;
                    } else {
                        result = (*result > _modes[index]) ? result : (_modes + index);
                    }
                }
                return result;
            }
            
            /**
             * @brief Picks the connector mode based on whether it's type 
             * is the preferred one. 
             * @return drmModeModeInfoPtr 
             */
            drmModeModeInfoPtr PreferredMode() const
            {
                drmModeModeInfoPtr result = nullptr;
                for (int index = 0; index < _modeCount; ++index) {
                    if (_modes[index].type & DRM_MODE_TYPE_PREFERRED) {
                        result = _modes + index;
                    }
                }
                return result;
            }

            /**
             * @brief Picks the first available mode, regardless of resolution or type.
             * @return drmModeModeInfoPtr 
             */
            drmModeModeInfoPtr DefaultMode() const
            {
                return _modes != nullptr ? _modes : nullptr;
            }

            ~DRMConnector()
            {
                drmModeFreeConnector(_hdmiConnector);
                drmModeFreeResources(_drmResources);
                close(_drmFd);
            }

        private:
            int _drmFd;
            drmModeConnector* _hdmiConnector;
            drmModeRes* _drmResources;
            const std::string& _drmDevice;
            drmModeModeInfoPtr _modes;
            int _modeCount;
        };
    }
}
}