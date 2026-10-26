package com.aniksarker.esptheater;

import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;

import java.io.IOException;

/**
 * Minimal USB serial drivers for what ESP boards use: CDC-ACM (ESP native USB, CH343/CH9102),
 * WCH CH340/CH341 and Silicon Labs CP210x. Protocols follow the Linux kernel drivers.
 */
abstract class UsbSerial implements SerialLink {
    static final int VID_ESPRESSIF = 0x303A;
    static final int VID_WCH = 0x1A86;
    static final int VID_SILABS = 0x10C4;

    final UsbDevice device;
    UsbDeviceConnection connection;
    UsbEndpoint in, out;

    UsbSerial(UsbDevice device) {
        this.device = device;
    }

    /** The driver for a device, or null if it's not a serial device we know. */
    static UsbSerial create(UsbDevice d) {
        if (CdcAcm.interfaces(d) != null) {
            return new CdcAcm(d);
        }
        if (d.getVendorId() == VID_WCH) {
            return new Ch34x(d);
        }
        if (d.getVendorId() == VID_SILABS) {
            return new Cp210x(d);
        }
        return null;
    }

    abstract void open(UsbManager manager) throws IOException;

    @Override
    public int read(byte[] buf, int timeoutMs) throws IOException {
        if (connection == null) {
            throw new IOException("Port closed");
        }
        int n = connection.bulkTransfer(in, buf, Math.min(buf.length, 16384), Math.max(1, timeoutMs));
        return Math.max(n, 0);
    }

    @Override
    public void write(byte[] data) throws IOException {
        int off = 0;
        while (off < data.length) {
            int n = connection.bulkTransfer(out, data, off, Math.min(16384, data.length - off), 3000);
            if (n <= 0) {
                throw new IOException("USB write failed (board disconnected?)");
            }
            off += n;
        }
    }

    void close() {
        if (connection != null) {
            connection.close();
            connection = null;
        }
    }

    UsbDeviceConnection openDevice(UsbManager manager) throws IOException {
        UsbDeviceConnection c = manager.openDevice(device);
        if (c == null) {
            throw new IOException("Cannot open the USB device (permission denied?)");
        }
        connection = c;
        return c;
    }

    void claim(UsbInterface i) throws IOException {
        if (!connection.claimInterface(i, true)) {
            throw new IOException("Cannot claim USB interface " + i.getId());
        }
    }

    void findBulkEndpoints(UsbInterface i) throws IOException {
        for (int e = 0; e < i.getEndpointCount(); e++) {
            UsbEndpoint ep = i.getEndpoint(e);
            if (ep.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK) {
                if (ep.getDirection() == UsbConstants.USB_DIR_IN) {
                    in = ep;
                } else {
                    out = ep;
                }
            }
        }
        if (in == null || out == null) {
            throw new IOException("USB serial endpoints not found");
        }
    }

    void controlOut(int requestType, int request, int value, int index, byte[] data) throws IOException {
        int len = data == null ? 0 : data.length;
        if (connection.controlTransfer(requestType, request, value, index, data, len, 1000) < 0) {
            throw new IOException(String.format("USB control request 0x%02X failed", request));
        }
    }

    int controlIn(int request, int value, int index, int length) {
        byte[] buf = new byte[length];
        return connection.controlTransfer(0xC0, request, value, index, buf, length, 1000);
    }

    static byte[] le32(int v) {
        return new byte[]{(byte) v, (byte) (v >> 8), (byte) (v >> 16), (byte) (v >> 24)};
    }

    // ---------------------------------------------------------------- CDC-ACM

    static final class CdcAcm extends UsbSerial {
        private UsbInterface comm, data;

        CdcAcm(UsbDevice d) {
            super(d);
        }

        /** {communication, data} interfaces of the first ACM function, or null. */
        static UsbInterface[] interfaces(UsbDevice d) {
            UsbInterface comm = null, data = null;
            for (int i = 0; i < d.getInterfaceCount() && comm == null; i++) {
                UsbInterface f = d.getInterface(i);
                if (f.getInterfaceClass() == UsbConstants.USB_CLASS_COMM && f.getInterfaceSubclass() == 2) {
                    comm = f;
                }
            }
            if (comm == null) {
                return null;
            }
            for (int i = 0; i < d.getInterfaceCount(); i++) {
                UsbInterface f = d.getInterface(i);
                if (f.getInterfaceClass() == UsbConstants.USB_CLASS_CDC_DATA && f.getEndpointCount() >= 2
                        && (data == null || f.getId() == comm.getId() + 1)) {
                    data = f;
                }
            }
            return data == null ? null : new UsbInterface[]{comm, data};
        }

        @Override
        void open(UsbManager manager) throws IOException {
            openDevice(manager);
            UsbInterface[] f = interfaces(device);
            comm = f[0];
            data = f[1];
            connection.claimInterface(comm, true);      // not required by every device
            claim(data);
            findBulkEndpoints(data);
        }

        @Override
        public void setBaudRate(int baud) throws IOException {
            byte[] coding = {(byte) baud, (byte) (baud >> 8), (byte) (baud >> 16), (byte) (baud >> 24), 0, 0, 8};
            controlOut(0x21, 0x20, 0, comm.getId(), coding);     // SET_LINE_CODING 8N1
        }

        @Override
        public void setLines(boolean dtr, boolean rts) throws IOException {
            controlOut(0x21, 0x22, (dtr ? 1 : 0) | (rts ? 2 : 0), comm.getId(), null);  // SET_CONTROL_LINE_STATE
        }
    }

    // ---------------------------------------------------------------- WCH CH340 / CH341

    static final class Ch34x extends UsbSerial {
        Ch34x(UsbDevice d) {
            super(d);
        }

        @Override
        void open(UsbManager manager) throws IOException {
            openDevice(manager);
            UsbInterface f = device.getInterface(0);
            claim(f);
            findBulkEndpoints(f);
            controlIn(0x5F, 0, 0, 2);                       // read version
            controlOut(0x40, 0xA1, 0, 0, null);             // serial init
            setBaudRate(115200);
            controlIn(0x95, 0x2518, 0, 2);
            controlOut(0x40, 0x9A, 0x2518, 0xC3, null);     // 8N1, RX/TX enabled
            controlIn(0x95, 0x0706, 0, 2);
            controlOut(0x40, 0xA1, 0x501F, 0xD90A, null);
            setBaudRate(115200);
            setLines(false, false);
        }

        @Override
        public void setBaudRate(int baud) throws IOException {
            long factor, divisor;
            if (baud == 921600) {
                divisor = 7;
                factor = 0xF300;
            } else {
                factor = 1532620800L / baud;
                divisor = 3;
                while (factor > 0xFFF0 && divisor > 0) {
                    factor >>= 3;
                    divisor--;
                }
                if (factor > 0xFFF0) {
                    throw new IOException("Unsupported baud rate " + baud);
                }
                factor = 0x10000 - factor;
            }
            divisor |= 0x0080;
            controlOut(0x40, 0x9A, 0x1312, (int) ((factor & 0xFF00) | divisor), null);
            controlOut(0x40, 0x9A, 0x0F2C, (int) (factor & 0xFF), null);
        }

        @Override
        public void setLines(boolean dtr, boolean rts) throws IOException {
            controlOut(0x40, 0xA4, ~((dtr ? 1 << 5 : 0) | (rts ? 1 << 6 : 0)) & 0xFFFF, 0, null);
        }
    }

    // ---------------------------------------------------------------- Silicon Labs CP210x

    static final class Cp210x extends UsbSerial {
        private int iface;

        Cp210x(UsbDevice d) {
            super(d);
        }

        @Override
        void open(UsbManager manager) throws IOException {
            openDevice(manager);
            UsbInterface f = device.getInterface(0);
            iface = f.getId();
            claim(f);
            findBulkEndpoints(f);
            controlOut(0x41, 0x00, 1, iface, null);         // IFC_ENABLE
            controlOut(0x41, 0x03, 0x0800, iface, null);    // SET_LINE_CTL 8N1
            setBaudRate(115200);
            setLines(false, false);
        }

        @Override
        public void setBaudRate(int baud) throws IOException {
            controlOut(0x41, 0x1E, 0, iface, le32(baud));   // SET_BAUDRATE
        }

        @Override
        public void setLines(boolean dtr, boolean rts) throws IOException {
            controlOut(0x41, 0x07, (dtr ? 1 : 0) | (rts ? 2 : 0) | 0x0300, iface, null);   // SET_MHS
        }
    }
}
