package com.aniksarker.esptheater;

import java.io.IOException;

/** A serial connection to the board. Implemented by the USB drivers (UsbSerial) and by test harnesses. */
public interface SerialLink {
    /** Reads up to buf.length bytes; returns 0 on timeout. */
    int read(byte[] buf, int timeoutMs) throws IOException;

    void write(byte[] data) throws IOException;

    void setBaudRate(int baud) throws IOException;

    /** Sets the DTR / RTS modem lines (on ESP boards: DTR drives GPIO0, RTS drives EN through transistors). */
    void setLines(boolean dtr, boolean rts) throws IOException;
}
