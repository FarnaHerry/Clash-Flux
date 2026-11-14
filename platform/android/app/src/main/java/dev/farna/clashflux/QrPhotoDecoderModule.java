package dev.farna.clashflux;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Rect;

import org.huxerui.HuxerUIFileReference;
import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/** Decodes camera bytes or a retained album reference on a worker thread. */
public final class QrPhotoDecoderModule implements HuxerUIPlatformModule.Factory {
    private static native byte[] decodePixels(Bitmap bitmap);

    @Override
    public HuxerUIPlatformModule create(Context context, PlatformPayload options,
                                        HuxerUIPlatformChannel.Events events) {
        return new Decoder(context.getApplicationContext());
    }

    private static final class Decoder implements HuxerUIPlatformModule {
        private final Context context;
        Decoder(Context context) { this.context = context; }
        private final ExecutorService executor = Executors.newSingleThreadExecutor();
        private final AtomicBoolean disposed = new AtomicBoolean(false);

        @Override
        public HuxerUIPlatformChannel.Cancellation invoke(
                String method, PlatformPayload arguments,
                HuxerUIPlatformChannel.Result result) {
            if (!"decode".equals(method) && !"decodeFile".equals(method)) {
                result.fail("qr/unknown-method", "Unknown QR decoder method.",
                        PlatformPayload.nullValue());
                return null;
            }

            final HuxerUIFileReference file = "decodeFile".equals(method) ? arguments.requireFileReference() : null;
            final byte[] imageBytes = file == null ? arguments.requireBytes() : null;
            AtomicBoolean cancelled = new AtomicBoolean(false);
            if (disposed.get()) {
                if (file != null) file.close();
                result.fail("qr/closed", "The QR decoder is closed.",
                        PlatformPayload.nullValue());
                return null;
            }
            try {
                executor.execute(() -> {
                    try {
                        if (cancelled.get() || disposed.get()) return;
                        String value = file == null ? decode(imageBytes) : decode(file);
                        if (!cancelled.get() && !disposed.get()) result.complete(PlatformPayload.string(value));
                    } catch (IOException | RuntimeException | OutOfMemoryError error) {
                        if (!cancelled.get() && !disposed.get()) {
                            result.fail("qr/decode-failed", error.getMessage() == null
                                            ? "Unable to decode the selected image." : error.getMessage(),
                                    PlatformPayload.nullValue());
                        }
                    } finally {
                        // Workers retain provider access until native reads end, including after Task cancellation.
                        if (file != null) file.close();
                    }
                });
            } catch (RuntimeException error) {
                if (file != null) file.close();
                result.fail("qr/closed", "The QR decoder is closed.", PlatformPayload.nullValue());
            }
            return () -> {
                cancelled.set(true);
                result.close();
            };
        }

        @Override
        public void dispose() {
            if (disposed.compareAndSet(false, true)) executor.shutdown();
        }

        private static String decode(byte[] jpeg) {
            if (jpeg.length == 0) return "";

            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length, bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return "";

            return decodeBitmap(BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length, sampleOptions(bounds)));
        }

        private String decode(HuxerUIFileReference file) throws IOException {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            try (InputStream input = open(file)) { BitmapFactory.decodeStream(input, null, bounds); }
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) throw new IOException("The selected image is unreadable");
            try (InputStream input = open(file)) {
                return decodeBitmap(BitmapFactory.decodeStream(input, null, sampleOptions(bounds)));
            }
        }

        private InputStream open(HuxerUIFileReference file) throws IOException {
            InputStream input = context.getContentResolver().openInputStream(file.uri());
            if (input == null) throw new IOException("The selected image could not be opened");
            return input;
        }

        private static BitmapFactory.Options sampleOptions(BitmapFactory.Options bounds) {
            int sample = 1;
            while ((bounds.outWidth - 1) / sample >= 1600
                    || (bounds.outHeight - 1) / sample >= 1600) {
                sample *= 2;
            }
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = sample;
            options.inPreferredConfig = Bitmap.Config.ARGB_8888;
            return options;
        }

        private static String decodeBitmap(Bitmap bitmap) {
            if (bitmap == null) return "";
            Bitmap pixels = bitmap;
            try {
                if (bitmap.getConfig() != Bitmap.Config.ARGB_8888 || bitmap.hasAlpha()) {
                    pixels = Bitmap.createBitmap(bitmap.getWidth(), bitmap.getHeight(),
                            Bitmap.Config.ARGB_8888);
                    Canvas canvas = new Canvas(pixels);
                    // The decoder ignores alpha. Composite transparent PNGs onto white.
                    canvas.drawColor(Color.WHITE);
                    canvas.drawBitmap(bitmap, null,
                            new Rect(0, 0, pixels.getWidth(), pixels.getHeight()), null);
                }
                return new String(decodePixels(pixels), StandardCharsets.UTF_8);
            } finally {
                if (pixels != bitmap) pixels.recycle();
                bitmap.recycle();
            }
        }
    }
}
