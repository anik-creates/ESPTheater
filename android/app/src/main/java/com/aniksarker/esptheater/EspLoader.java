package com.aniksarker.esptheater;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.util.zip.Deflater;

/**
 * Talks to the ESP ROM bootloader without a flasher stub, using Espressif's documented serial protocol
 * (docs.espressif.com/projects/esptool -> Serial Protocol): SLIP framing, sync, chip detection, compressed
 * flashing, MD5 verify and the reset sequences (constants match esptool). Plain Java, testable off-device.
 */
public class EspLoader {

    /** How the board is attached, which decides the reset sequences. */
    public enum Link {
        UART_BRIDGE,    // CH34x / CP210x / ... with the usual DTR/RTS auto-reset circuit
        USB_JTAG,       // ESP32-S3 USB-Serial/JTAG (303A:1001)
        USB_OTG_ROM,    // ROM download mode over native USB-OTG CDC, e.g. ESP32-S2 (303A:0002)
    }

    public interface Listener {
        void log(String message);

        void progress(int percent);
    }

    public static class EspException extends IOException {
        public EspException(String message) {
            super(message);
        }
    }

    public static final class Chip {
        public final String name;
        public final String id;         // firmware asset prefix, e.g. "esp32s3"; null if unsupported
        public final boolean supported;

        Chip(String name, String id, boolean supported) {
            this.name = name;
            this.id = id;
            this.supported = supported;
        }
    }

    // Commands
    private static final int FLASH_BEGIN = 0x02;
    private static final int SYNC = 0x08;
    private static final int WRITE_REG = 0x09;
    private static final int READ_REG = 0x0A;
    private static final int SPI_SET_PARAMS = 0x0B;
    private static final int SPI_ATTACH = 0x0D;
    private static final int CHANGE_BAUDRATE = 0x0F;
    private static final int FLASH_DEFL_BEGIN = 0x10;
    private static final int FLASH_DEFL_DATA = 0x11;
    private static final int SPI_FLASH_MD5 = 0x13;
    private static final int GET_SECURITY_INFO = 0x14;

    private static final int CHIP_DETECT_MAGIC_REG = 0x40001000;
    private static final int FLASH_WRITE_SIZE = 0x400;      // ROM loader block size
    private static final int DEFAULT_TIMEOUT = 3000;
    private static final int ERASE_MS_PER_MB = 30000;
    private static final int MD5_MS_PER_MB = 8000;
    public static final int FLASH_SIZE = 4 * 1024 * 1024;  // the images are built for 4 MB

    private final SerialLink port;
    private final Link link;
    private final Listener listener;
    private final byte[] rx = new byte[4096];
    private int rxPos, rxLen;
    private Chip chip;
    private int rtcWdtBase;     // set by detectChip() for chips that support the watchdog reset

    public EspLoader(SerialLink port, Link link, Listener listener) {
        this.port = port;
        this.link = link;
        this.listener = listener;
    }

    // ------------------------------------------------------------------ connection

    /** Resets the board into its bootloader (if the link allows it) and syncs with the ROM. */
    public void connect() throws IOException {
        for (int attempt = 0; attempt < 8; attempt++) {
            boolean reset = !(link == Link.USB_OTG_ROM || (link == Link.USB_JTAG && attempt == 0));
            if (reset) {
                if (link == Link.USB_JTAG) {
                    usbJtagReset();
                } else {
                    classicReset(attempt % 2 == 0 ? 50 : 550);
                }
            }
            flushInput();
            for (int i = 0; i < 5; i++) {
                if (trySync()) {
                    return;
                }
            }
            if (link == Link.USB_OTG_ROM && attempt >= 2) {
                break;
            }
        }
        throw new EspException("No response from the board's bootloader.");
    }

    private boolean trySync() throws IOException {
        byte[] data = new byte[36];
        data[0] = 0x07;
        data[1] = 0x07;
        data[2] = 0x12;
        data[3] = 0x20;
        for (int i = 4; i < 36; i++) {
            data[i] = 0x55;
        }
        sendCommand(SYNC, data, 0);
        Response r = readResponse(SYNC, 100);
        if (r == null) {
            return false;
        }
        for (int i = 0; i < 7; i++) {   // the ROM answers a sync several times
            readResponse(SYNC, 50);
        }
        flushInput();
        return true;
    }

    private void classicReset(int releaseDelayMs) throws IOException {
        port.setLines(false, true);     // IO0 high, EN low: chip in reset
        sleep(100);
        port.setLines(true, false);     // IO0 low, EN high: boots into the download mode
        sleep(releaseDelayMs);
        port.setLines(false, false);
    }

    private void usbJtagReset() throws IOException {
        port.setLines(false, false);
        sleep(100);
        port.setLines(true, false);     // IO0
        sleep(100);
        port.setLines(true, true);      // reset, passing through (1,1) rather than (0,0) like esptool
        port.setLines(false, true);
        sleep(100);
        port.setLines(false, false);
    }

    /** Leaves the bootloader and starts the flashed firmware. */
    public void hardReset() {
        try {
            if (link == Link.USB_OTG_ROM && rtcWdtBase != 0) {
                watchdogReset();
            } else if (link == Link.USB_JTAG) {
                port.setLines(false, true);
                sleep(200);
                port.setLines(false, false);
                sleep(200);
            } else {
                port.setLines(false, true);
                sleep(100);
                port.setLines(false, false);
            }
        } catch (IOException e) {
            // the board usually drops off the bus while it restarts
        }
    }

    private void watchdogReset() throws IOException {
        int wprotect = rtcWdtBase == 0x3F408000 ? 0xAC : 0xB0;
        int config0 = rtcWdtBase == 0x3F408000 ? 0x94 : 0x98;
        int config1 = config0 + 4;
        writeReg(rtcWdtBase + wprotect, 0x50D83AA1);    // unlock
        writeReg(rtcWdtBase + config1, 2000);           // timeout
        writeReg(rtcWdtBase + config0, (1 << 31) | (5 << 28) | (1 << 8) | 2);
        try {
            writeReg(rtcWdtBase + wprotect, 0);         // lock; the chip may already be resetting
        } catch (IOException ignored) {
        }
    }

    // ------------------------------------------------------------------ chip detection

    public Chip detectChip() throws IOException {
        Integer chipId = null;
        sendCommand(GET_SECURITY_INFO, new byte[0], 0);
        Response r = readResponse(GET_SECURITY_INFO, DEFAULT_TIMEOUT);
        if (r != null && r.data.length >= 22 && r.data[20] == 0) {
            chipId = le32(r.data, 12);                  // ESP32-S3 and newer report a chip ID
        } else if (r == null || r.data.length < 14 || r.data[12] != 0) {
            drainErrors();                              // unsupported command (ESP32, ESP8266)
        }
        if (chipId != null) {
            switch (chipId) {
                case 9:  chip = new Chip("ESP32-S3", "esp32s3", true); rtcWdtBase = 0x60008000; break;
                case 5:  chip = new Chip("ESP32-C3", null, false); break;
                case 12: chip = new Chip("ESP32-C2", null, false); break;
                case 13: chip = new Chip("ESP32-C6", null, false); break;
                case 16: chip = new Chip("ESP32-H2", null, false); break;
                case 18: chip = new Chip("ESP32-P4", null, false); break;
                case 23: chip = new Chip("ESP32-C5", null, false); break;
                default: chip = new Chip("ESP chip (id " + chipId + ")", null, false); break;
            }
            return chip;
        }
        int magic = readReg(CHIP_DETECT_MAGIC_REG);
        switch (magic) {
            case 0x000007C6: chip = new Chip("ESP32-S2", "esp32s2", true); rtcWdtBase = 0x3F408000; break;
            case 0x00F01D83: chip = new Chip("ESP32", null, false); break;
            case 0xFFF0C101: chip = new Chip("ESP8266", null, false); break;
            case 0x6921506F: case 0x1B31506F: case 0x4881606F: case 0x4361606F:
                chip = new Chip("ESP32-C3", null, false); break;
            case 0x6F51306F: case 0x7C41A06F:
                chip = new Chip("ESP32-C2", null, false); break;
            default: chip = new Chip(String.format("unknown ESP chip (magic 0x%08X)", magic), null, false); break;
        }
        return chip;
    }

    // ------------------------------------------------------------------ flashing

    /** Switches a UART bridge to a faster baud rate (pointless on native USB). */
    public void changeBaud(int baud) throws IOException {
        if (link != Link.UART_BRIDGE) {
            return;
        }
        sendCommand(CHANGE_BAUDRATE, pack(baud, 0), 0);
        readResponse(CHANGE_BAUDRATE, DEFAULT_TIMEOUT);
        port.setBaudRate(baud);
        sleep(50);
        flushInput();
        listener.log("Speed " + baud + " baud.");
    }

    /** Writes image at offset (compressed), then verifies it with the ROM's MD5. */
    public void writeFlash(byte[] image, int offset) throws IOException {
        checkCommand("attach SPI flash", SPI_ATTACH, new byte[8], 0, 0, DEFAULT_TIMEOUT);
        checkCommand("set flash parameters", SPI_SET_PARAMS,
                pack(0, FLASH_SIZE, 64 * 1024, 4 * 1024, 256, 0xFFFF), 0, 0, DEFAULT_TIMEOUT);

        byte[] compressed = deflate(image);
        int blocks = (compressed.length + FLASH_WRITE_SIZE - 1) / FLASH_WRITE_SIZE;
        int eraseSize = (image.length + FLASH_WRITE_SIZE - 1) / FLASH_WRITE_SIZE * FLASH_WRITE_SIZE;
        listener.log(String.format("Erasing and writing %,d bytes (%,d compressed) at 0x%X...",
                image.length, compressed.length, offset));
        int eraseTimeout = Math.max(10000, (int) ((long) ERASE_MS_PER_MB * eraseSize / (1024 * 1024)) + 5000);
        checkCommand("erase flash", FLASH_DEFL_BEGIN,
                pack(eraseSize, blocks, FLASH_WRITE_SIZE, offset, 0), 0, 0, eraseTimeout);
        listener.progress(5);

        for (int seq = 0; seq < blocks; seq++) {
            int start = seq * FLASH_WRITE_SIZE;
            int len = Math.min(FLASH_WRITE_SIZE, compressed.length - start);
            byte[] payload = new byte[16 + len];
            System.arraycopy(pack(len, seq, 0, 0), 0, payload, 0, 16);
            System.arraycopy(compressed, start, payload, 16, len);
            checkCommand("write block " + seq, FLASH_DEFL_DATA, payload, checksum(compressed, start, len), 0, 10000);
            listener.progress(5 + (int) (90L * (seq + 1) / blocks));
        }

        listener.log("Verifying...");
        int md5Timeout = Math.max(5000, (int) ((long) MD5_MS_PER_MB * image.length / (1024 * 1024)) + 3000);
        byte[] md5 = checkCommand("verify", SPI_FLASH_MD5, pack(offset, image.length, 0, 0), 0, 32, md5Timeout);
        String expected = hex(md5(image));
        String actual = new String(md5, "US-ASCII").toLowerCase();
        if (!expected.equals(actual)) {
            throw new EspException("Verification failed: the flash content doesn't match (MD5 " + actual + ").");
        }
        listener.progress(100);
        listener.log("Verified (MD5 " + expected + ").");
    }

    public int readReg(int addr) throws IOException {
        sendCommand(READ_REG, pack(addr), 0);
        Response r = expect(READ_REG, DEFAULT_TIMEOUT, "read register");
        checkStatus(r, 0, "read register");
        return r.value;
    }

    private void writeReg(int addr, int value) throws IOException {
        checkCommand("write register", WRITE_REG, pack(addr, value, 0xFFFFFFFF, 0), 0, 0, DEFAULT_TIMEOUT);
    }

    // ------------------------------------------------------------------ protocol

    private static final class Response {
        int op;
        int value;
        byte[] data;
    }

    private byte[] checkCommand(String what, int op, byte[] data, int chk, int respLen, int timeoutMs)
            throws IOException {
        sendCommand(op, data, chk);
        Response r = expect(op, timeoutMs, what);
        checkStatus(r, respLen, what);
        byte[] out = new byte[respLen];
        System.arraycopy(r.data, 0, out, 0, respLen);
        return out;
    }

    private static void checkStatus(Response r, int respLen, String what) throws EspException {
        int at = r.data.length >= respLen + 2 ? respLen : 0;
        if (r.data.length < at + 2 || r.data[at] != 0) {
            int reason = r.data.length >= at + 2 ? r.data[at + 1] & 0xFF : -1;
            throw new EspException(String.format("Failed to %s (error 0x%02X).", what, reason));
        }
    }

    private Response expect(int op, int timeoutMs, String what) throws IOException {
        Response r = readResponse(op, timeoutMs);
        if (r == null) {
            throw new EspException("Timed out waiting for the board to " + what + ".");
        }
        return r;
    }

    private void sendCommand(int op, byte[] data, int chk) throws IOException {
        byte[] pkt = new byte[8 + data.length];
        pkt[0] = 0;
        pkt[1] = (byte) op;
        pkt[2] = (byte) data.length;
        pkt[3] = (byte) (data.length >> 8);
        System.arraycopy(pack(chk), 0, pkt, 4, 4);
        System.arraycopy(data, 0, pkt, 8, data.length);
        port.write(slipEncode(pkt));
    }

    /** Reads packets until a response to op arrives; null on timeout. */
    private Response readResponse(int op, int timeoutMs) throws IOException {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (true) {
            byte[] p = readPacket(deadline);
            if (p == null) {
                return null;
            }
            if (p.length < 8 || p[0] != 1) {
                continue;
            }
            Response r = new Response();
            r.op = p[1] & 0xFF;
            r.value = le32(p, 4);
            r.data = new byte[p.length - 8];
            System.arraycopy(p, 8, r.data, 0, r.data.length);
            if (r.op == op) {
                return r;
            }
        }
    }

    /** Unsupported commands are answered with several error packets: swallow them. */
    private void drainErrors() throws IOException {
        sleep(200);
        flushInput();
    }

    private byte[] readPacket(long deadline) throws IOException {
        ByteArrayOutputStream pkt = new ByteArrayOutputStream();
        boolean inPacket = false, escape = false;
        while (true) {
            if (rxPos >= rxLen) {
                long left = deadline - System.currentTimeMillis();
                if (left <= 0) {
                    return null;
                }
                rxLen = port.read(rx, (int) Math.min(left, 500));
                rxPos = 0;
                continue;
            }
            int b = rx[rxPos++] & 0xFF;
            if (!inPacket) {
                if (b == 0xC0) {
                    inPacket = true;
                }
            } else if (escape) {
                escape = false;
                pkt.write(b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b);
            } else if (b == 0xDB) {
                escape = true;
            } else if (b == 0xC0) {
                if (pkt.size() > 0) {
                    return pkt.toByteArray();
                }
                // empty packet: treat as the start of the next one
            } else {
                pkt.write(b);
            }
        }
    }

    private void flushInput() throws IOException {
        rxPos = rxLen = 0;
        byte[] junk = new byte[4096];
        for (int i = 0; i < 50 && port.read(junk, 10) > 0; i++) {
        }
    }

    // ------------------------------------------------------------------ helpers

    private static byte[] slipEncode(byte[] data) {
        ByteArrayOutputStream out = new ByteArrayOutputStream(data.length + 16);
        out.write(0xC0);
        for (byte d : data) {
            int b = d & 0xFF;
            if (b == 0xC0) {
                out.write(0xDB);
                out.write(0xDC);
            } else if (b == 0xDB) {
                out.write(0xDB);
                out.write(0xDD);
            } else {
                out.write(b);
            }
        }
        out.write(0xC0);
        return out.toByteArray();
    }

    private static byte[] pack(int... words) {
        byte[] out = new byte[words.length * 4];
        for (int i = 0; i < words.length; i++) {
            out[i * 4] = (byte) words[i];
            out[i * 4 + 1] = (byte) (words[i] >> 8);
            out[i * 4 + 2] = (byte) (words[i] >> 16);
            out[i * 4 + 3] = (byte) (words[i] >> 24);
        }
        return out;
    }

    private static int le32(byte[] b, int off) {
        return (b[off] & 0xFF) | (b[off + 1] & 0xFF) << 8 | (b[off + 2] & 0xFF) << 16 | (b[off + 3] & 0xFF) << 24;
    }

    private static int checksum(byte[] data, int off, int len) {
        int chk = 0xEF;
        for (int i = off; i < off + len; i++) {
            chk ^= data[i] & 0xFF;
        }
        return chk;
    }

    private static byte[] deflate(byte[] data) {
        Deflater d = new Deflater(Deflater.BEST_COMPRESSION);   // zlib format, as the ROM expects
        d.setInput(data);
        d.finish();
        ByteArrayOutputStream out = new ByteArrayOutputStream(data.length / 2);
        byte[] buf = new byte[65536];
        while (!d.finished()) {
            out.write(buf, 0, d.deflate(buf));
        }
        d.end();
        return out.toByteArray();
    }

    private static byte[] md5(byte[] data) {
        try {
            return MessageDigest.getInstance("MD5").digest(data);
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    private static String hex(byte[] b) {
        StringBuilder s = new StringBuilder();
        for (byte x : b) {
            s.append(String.format("%02x", x & 0xFF));
        }
        return s.toString();
    }

    static void sleep(int ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
