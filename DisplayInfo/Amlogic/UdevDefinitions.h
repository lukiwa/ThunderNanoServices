#pragma once

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
