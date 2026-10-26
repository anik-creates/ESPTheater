package com.aniksarker.esptheater;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.method.ScrollingMovementMethod;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** ESPTheater Flasher: flashes the ESP USB Wi-Fi Adapter firmware from an Android phone over USB-OTG. */
public class MainActivity extends Activity {

    static final String VERSION = "1.0.0";
    static final String AUTHOR = "Anik Sarker";
    static final String REPO_URL = "https://github.com/anik-creates/ESPTheater";
    private static final String ACTION_USB_PERMISSION = "com.aniksarker.esptheater.USB_PERMISSION";
    private static final int FLASH_BAUD = 460800;

    private static final int C_BG = 0xFFF4F5F7, C_CARD = 0xFFFFFFFF, C_LINE = 0xFFE5E7EB;
    private static final int C_TEXT = 0xFF1D2129, C_MUTED = 0xFF6B7280, C_ACCENT = 0xFF2563EB;

    private UsbManager usb;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final List<UsbDevice> devices = new ArrayList<>();
    private Spinner deviceSpinner;
    private TextView deviceHint, statusText, logText;
    private RadioButton fullRadio, updateRadio;
    private Button flashButton, detectButton, refreshButton;
    private ProgressBar progress;
    private volatile boolean busy;

    private volatile CountDownLatch permissionLatch;
    private volatile boolean permissionGranted;

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (ACTION_USB_PERMISSION.equals(action)) {
                permissionGranted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                CountDownLatch latch = permissionLatch;
                if (latch != null) {
                    latch.countDown();
                }
            } else if (!busy) {
                refreshDevices();   // attached / detached
            }
        }
    };

    // ------------------------------------------------------------------ lifecycle

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        usb = (UsbManager) getSystemService(Context.USB_SERVICE);
        setTitle("ESPTheater Flasher");
        setContentView(buildUi());

        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(receiver, filter);
        }
        log("ESPTheater Flasher v" + VERSION + "  -  developed by " + AUTHOR);
        log("1) Connect the ESP32-S3 / ESP32-S2 board with a USB-OTG cable or adapter.");
        log("2) Pick it below and tap Flash.");
        log("");
        refreshDevices();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (!busy) {
            refreshDevices();   // launched / re-launched by plugging a board in
        }
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(receiver);
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (busy) {
            new AlertDialog.Builder(this)
                    .setMessage("Flashing is in progress. Leaving now can leave the board unusable until it is flashed again.")
                    .setPositiveButton("Stay", null)
                    .setNegativeButton("Leave", (d, w) -> MainActivity.super.onBackPressed())
                    .show();
        } else {
            super.onBackPressed();
        }
    }

    // ------------------------------------------------------------------ UI

    private int dp(float v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private LinearLayout card(LinearLayout parent, String title) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(16), dp(14), dp(16), dp(14));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(C_CARD);
        bg.setCornerRadius(dp(12));
        bg.setStroke(dp(1), C_LINE);
        c.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(12);
        parent.addView(c, lp);
        if (title != null) {
            TextView t = text(title, 15, C_TEXT);
            t.setTypeface(Typeface.DEFAULT_BOLD);
            t.setPadding(0, 0, 0, dp(6));
            c.addView(t);
        }
        return c;
    }

    private TextView text(String s, float sp, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        return t;
    }

    private View buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(C_BG);
        root.setFitsSystemWindows(true);

        // Watermark behind everything
        TextView mark = text(AUTHOR, 54, C_TEXT);
        mark.setTypeface(Typeface.DEFAULT_BOLD);
        mark.setAlpha(0.05f);
        mark.setRotation(-30);
        root.addView(mark, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));

        ScrollView scroll = new ScrollView(this);
        root.addView(scroll);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(16), dp(8), dp(16), dp(24));
        scroll.addView(col);

        TextView title = text("Turn an ESP32-S3 / ESP32-S2 board into a USB Wi-Fi adapter", 20, C_TEXT);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setPadding(0, dp(8), 0, dp(2));
        col.addView(title);
        col.addView(text("ESPTheater  ·  developed by " + AUTHOR, 13, C_MUTED));

        // 1. Board
        LinearLayout board = card(col, "1. Board");
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        deviceSpinner = new Spinner(this);
        row.addView(deviceSpinner, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        refreshButton = new Button(this);
        refreshButton.setText("Refresh");
        refreshButton.setOnClickListener(v -> refreshDevices());
        row.addView(refreshButton);
        board.addView(row);
        deviceHint = text("", 13, C_MUTED);
        deviceHint.setPadding(0, dp(4), 0, 0);
        board.addView(deviceHint);
        deviceSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                updateHint();
            }

            @Override
            public void onNothingSelected(AdapterView<?> p) {
                updateHint();
            }
        });

        // 2. Mode
        LinearLayout mode = card(col, "2. Mode");
        RadioGroup group = new RadioGroup(this);
        fullRadio = new RadioButton(this);
        fullRadio.setText("Full install  (new board or start over; clears saved Wi-Fi and settings)");
        fullRadio.setId(View.generateViewId());
        updateRadio = new RadioButton(this);
        updateRadio.setText("Update firmware  (keeps saved Wi-Fi and settings)");
        updateRadio.setId(View.generateViewId());
        group.addView(fullRadio);
        group.addView(updateRadio);
        fullRadio.setChecked(true);
        mode.addView(group);

        // 3. Flash
        LinearLayout actions = card(col, "3. Flash");
        LinearLayout buttons = new LinearLayout(this);
        flashButton = new Button(this);
        flashButton.setText("Flash");
        flashButton.setTextColor(Color.WHITE);
        GradientDrawable fb = new GradientDrawable();
        fb.setColor(C_ACCENT);
        fb.setCornerRadius(dp(8));
        flashButton.setBackground(fb);
        flashButton.setOnClickListener(v -> start(false));
        detectButton = new Button(this);
        detectButton.setText("Detect board");
        detectButton.setOnClickListener(v -> start(true));
        LinearLayout.LayoutParams half = new LinearLayout.LayoutParams(0, dp(48), 1);
        half.rightMargin = dp(8);
        buttons.addView(flashButton, half);
        buttons.addView(detectButton, new LinearLayout.LayoutParams(0, dp(48), 1));
        actions.addView(buttons);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(16));
        plp.topMargin = dp(12);
        actions.addView(progress, plp);
        statusText = text("Connect the board and pick it above.", 14, C_TEXT);
        actions.addView(statusText);

        // Log
        LinearLayout logCard = card(col, "Log");
        logText = text("", 11, C_TEXT);
        logText.setTypeface(Typeface.MONOSPACE);
        logText.setTextIsSelectable(true);
        logText.setMovementMethod(new ScrollingMovementMethod());
        logText.setMaxLines(18);
        logText.setVerticalScrollBarEnabled(true);
        logCard.addView(logText);

        // Footer
        LinearLayout footer = card(col, null);
        LinearLayout links = new LinearLayout(this);
        Button setup = new Button(this);
        setup.setText("Open setup page");
        setup.setOnClickListener(v -> new AlertDialog.Builder(this)
                .setTitle("Setup page")
                .setMessage("Join the Wi-Fi hotspot \"ESP-USB-WiFi-XXXX\" that the board opens, then open the page. "
                        + "(From a PC connected to the board's USB port, use http://192.168.7.1 instead.)")
                .setPositiveButton("Open http://192.168.4.1", (d, w) -> openUrl("http://192.168.4.1/"))
                .setNegativeButton("Cancel", null)
                .show());
        Button help = new Button(this);
        help.setText("Help");
        help.setOnClickListener(v -> showHelp());
        links.addView(setup, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        links.addView(help, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        footer.addView(links);
        TextView credit = text("ESPTheater v" + VERSION + "  ·  © 2026 " + AUTHOR + "\n" + REPO_URL, 12, C_MUTED);
        credit.setGravity(Gravity.CENTER);
        credit.setPadding(0, dp(8), 0, 0);
        credit.setOnClickListener(v -> openUrl(REPO_URL));
        footer.addView(credit);
        TextView why = text("Supported: ESP32-S3, ESP32-S2. Not possible: ESP32, ESP32-C3/C6, ESP8266 (no USB device hardware).",
                12, C_MUTED);
        why.setGravity(Gravity.CENTER);
        why.setPadding(0, dp(6), 0, 0);
        footer.addView(why);
        return root;
    }

    private void showHelp() {
        new AlertDialog.Builder(this)
                .setTitle("Help")
                .setMessage("Connecting\n"
                        + "Use a USB-OTG adapter or a USB-C-to-C cable. Boards with two USB ports: either works for flashing.\n\n"
                        + "Android asks to allow access to the USB device: tap OK (tick \"always\" to skip it next time).\n\n"
                        + "\"No response from the bootloader\"\n"
                        + "Hold BOOT, tap RESET (or plug in while holding BOOT), release BOOT, tap Refresh and Flash again.\n\n"
                        + "After flashing\n"
                        + "Connect the board's native USB port (often labelled \"USB\") to your PC. It shows up as a network adapter. "
                        + "Pick your Wi-Fi on the hotspot \"ESP-USB-WiFi-XXXX\" or at http://192.168.7.1 from the PC.\n\n"
                        + "LED\n"
                        + "Blue blink: choose Wi-Fi.  Yellow: connecting.  Red: no internet.  Cyan: online, no PC.  Green: all good.")
                .setPositiveButton("OK", null)
                .show();
    }

    private void openUrl(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception ignored) {
        }
    }

    // ------------------------------------------------------------------ devices

    static String describe(UsbDevice d) {
        int vid = d.getVendorId(), pid = d.getProductId();
        String name;
        if (vid == UsbSerial.VID_ESPRESSIF) {
            name = pid == 0x4001 ? "ESP USB Wi-Fi Adapter (running)"
                 : pid == 0x1001 ? "ESP32-S3 native USB (serial/JTAG)"
                 : pid == 0x0002 ? "ESP native USB bootloader"
                 : "ESP native USB";
        } else if (vid == UsbSerial.VID_WCH) {
            name = "CH34x USB-UART bridge";
        } else if (vid == UsbSerial.VID_SILABS) {
            name = "CP210x USB-UART bridge";
        } else {
            name = d.getProductName() != null ? d.getProductName() : "USB serial device";
        }
        return String.format("%s  [%04X:%04X]", name, vid, pid);
    }

    private void refreshDevices() {
        UsbDevice previous = selectedDevice();
        devices.clear();
        for (UsbDevice d : usb.getDeviceList().values()) {
            if (UsbSerial.create(d) != null) {
                devices.add(d);
            }
        }
        List<String> labels = new ArrayList<>();
        for (UsbDevice d : devices) {
            labels.add(describe(d));
        }
        if (labels.isEmpty()) {
            labels.add("No board found");
        }
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, labels);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        deviceSpinner.setAdapter(adapter);
        for (int i = 0; i < devices.size(); i++) {
            if (previous != null && devices.get(i).getDeviceName().equals(previous.getDeviceName())) {
                deviceSpinner.setSelection(i);
            }
        }
        updateHint();
    }

    private UsbDevice selectedDevice() {
        int i = deviceSpinner == null ? -1 : deviceSpinner.getSelectedItemPosition();
        return i >= 0 && i < devices.size() ? devices.get(i) : null;
    }

    private void updateHint() {
        UsbDevice d = selectedDevice();
        if (d == null) {
            deviceHint.setText("Plug the board in with a USB-OTG cable/adapter, then tap Refresh.");
        } else if (d.getVendorId() == UsbSerial.VID_ESPRESSIF && d.getProductId() == 0x4001) {
            deviceHint.setText("The adapter firmware is running - it will be switched to its bootloader automatically.");
        } else if (d.getVendorId() == UsbSerial.VID_ESPRESSIF) {
            deviceHint.setText("ESP native USB - ready. If it doesn't respond, hold BOOT, tap RESET, release BOOT.");
        } else {
            deviceHint.setText("USB-UART bridge - ready to flash.");
        }
    }

    // ------------------------------------------------------------------ flashing

    private void log(String s) {
        ui.post(() -> {
            logText.append(s + "\n");
            int scroll = logText.getLayout() == null ? 0
                    : logText.getLayout().getLineTop(logText.getLineCount()) - logText.getHeight();
            if (scroll > 0) {
                logText.scrollTo(0, scroll);
            }
        });
    }

    private void status(String s) {
        ui.post(() -> statusText.setText(s));
    }

    private void setBusy(boolean b) {
        busy = b;
        ui.post(() -> {
            flashButton.setEnabled(!b);
            detectButton.setEnabled(!b);
            refreshButton.setEnabled(!b);
            deviceSpinner.setEnabled(!b);
            fullRadio.setEnabled(!b);
            updateRadio.setEnabled(!b);
            if (b) {
                getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            } else {
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            }
        });
    }

    private void start(boolean detectOnly) {
        UsbDevice d = selectedDevice();
        if (d == null) {
            refreshDevices();
            d = selectedDevice();
            if (d == null) {
                new AlertDialog.Builder(this).setMessage("No board found. Connect it with a USB-OTG cable or adapter.")
                        .setPositiveButton("OK", null).show();
                return;
            }
        }
        final UsbDevice device = d;
        final boolean update = updateRadio.isChecked();
        progress.setProgress(0);
        log("---- " + (detectOnly ? "detect" : update ? "update" : "full install") + " on " + describe(device));
        setBusy(true);
        new Thread(() -> {
            String error = null;
            String done = null;
            try {
                done = runJob(device, update, detectOnly);
            } catch (Exception e) {
                error = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final String err = error, ok = done;
            setBusy(false);
            ui.post(() -> finish(ok, err));
        }, "flasher").start();
    }

    private void finish(String ok, String error) {
        refreshDevices();
        if (error != null) {
            status("Failed.");
            log("ERROR: " + error);
            new AlertDialog.Builder(this).setTitle("Flashing failed").setMessage(error).setPositiveButton("OK", null).show();
        } else {
            status(ok);
            log(ok);
            new AlertDialog.Builder(this).setTitle("Done").setMessage(ok).setPositiveButton("OK", null).show();
        }
    }

    /** Returns a success message, or throws with a user-readable error. */
    private String runJob(UsbDevice device, boolean update, boolean detectOnly) throws Exception {
        status("Waiting for USB permission...");
        if (!ensurePermission(device)) {
            throw new IOException("USB permission was not granted.");
        }
        int vid = device.getVendorId(), pid = device.getProductId();
        if (vid == UsbSerial.VID_ESPRESSIF && pid != 0x1001 && pid != 0x0002) {
            status("Switching the board to its bootloader...");
            log("Asking the running firmware to enter its bootloader...");
            touch1200(device);
            UsbDevice boot = waitForBootloader(15000);
            if (boot == null) {
                throw new IOException("The board did not enter its bootloader. Hold BOOT, tap RESET (or re-plug while holding BOOT), "
                        + "release BOOT, tap Refresh and try again.");
            }
            log("Bootloader: " + describe(boot));
            device = boot;
            if (!ensurePermission(device)) {
                throw new IOException("USB permission for the bootloader was not granted.");
            }
            vid = device.getVendorId();
            pid = device.getProductId();
        }
        EspLoader.Link link = vid != UsbSerial.VID_ESPRESSIF ? EspLoader.Link.UART_BRIDGE
                : pid == 0x1001 ? EspLoader.Link.USB_JTAG : EspLoader.Link.USB_OTG_ROM;

        UsbSerial port = UsbSerial.create(device);
        if (port == null) {
            throw new IOException("This USB device is not a supported serial port.");
        }
        try {
            port.open(usb);
            port.setBaudRate(115200);
            EspLoader esp = new EspLoader(port, link, new EspLoader.Listener() {
                @Override
                public void log(String m) {
                    MainActivity.this.log(m);
                }

                @Override
                public void progress(int p) {
                    ui.post(() -> progress.setProgress(p));
                }
            });
            status("Connecting to the bootloader...");
            esp.connect();
            EspLoader.Chip chip = esp.detectChip();
            log("Detected " + chip.name + ".");
            if (!chip.supported) {
                esp.hardReset();
                throw new IOException("This board has an " + chip.name + ", which can't be used.\n\n"
                        + "Only the ESP32-S2 and ESP32-S3 have the USB device hardware needed to act as a network card "
                        + "(on the others the USB port goes to a USB-to-serial chip).");
            }
            if (detectOnly) {
                esp.hardReset();
                return "This board has an " + chip.name + " - supported. Tap Flash to install.";
            }
            byte[] image = readAsset("firmware/" + chip.id + (update ? "-app.bin" : "-full.bin"));
            status(update ? "Updating..." : "Installing...");
            esp.changeBaud(FLASH_BAUD);
            esp.writeFlash(image, update ? 0x10000 : 0);
            status("Restarting the board...");
            esp.hardReset();
        } finally {
            port.close();
        }
        return "Flashed successfully. Now connect the board's native USB port to your PC, and choose your Wi-Fi network on "
                + "the hotspot \"ESP-USB-WiFi-XXXX\" (or at http://192.168.7.1 from the PC).";
    }

    /** Arduino "1200 baud touch": the adapter firmware reboots into the ROM bootloader. */
    private void touch1200(UsbDevice device) {
        UsbSerial port = UsbSerial.create(device);
        try {
            port.open(usb);
            port.setBaudRate(1200);
            port.setLines(false, false);
        } catch (IOException ignored) {
            // the board drops off the bus as it reboots
        } finally {
            port.close();
        }
    }

    private UsbDevice waitForBootloader(long timeoutMs) {
        long deadline = System.currentTimeMillis() + timeoutMs;
        EspLoader.sleep(1000);
        while (System.currentTimeMillis() < deadline) {
            for (UsbDevice d : usb.getDeviceList().values()) {
                if (d.getVendorId() == UsbSerial.VID_ESPRESSIF && (d.getProductId() == 0x1001 || d.getProductId() == 0x0002)) {
                    EspLoader.sleep(500);
                    return d;
                }
            }
            EspLoader.sleep(300);
        }
        return null;
    }

    private boolean ensurePermission(UsbDevice device) throws InterruptedException {
        if (usb.hasPermission(device)) {
            return true;
        }
        permissionGranted = false;
        permissionLatch = new CountDownLatch(1);
        int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
        Intent intent = new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName());
        PendingIntent pi = PendingIntent.getBroadcast(this, 0, intent, flags | PendingIntent.FLAG_UPDATE_CURRENT);
        ui.post(() -> usb.requestPermission(device, pi));
        permissionLatch.await(120, TimeUnit.SECONDS);
        return permissionGranted || usb.hasPermission(device);
    }

    private byte[] readAsset(String name) throws IOException {
        try (InputStream in = getAssets().open(name)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        }
    }
}
