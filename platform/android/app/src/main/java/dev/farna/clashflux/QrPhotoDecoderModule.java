package dev.farna.clashflux;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import com.google.zxing.BarcodeFormat;
import com.google.zxing.BinaryBitmap;
import com.google.zxing.DecodeHintType;
import com.google.zxing.MultiFormatReader;
import com.google.zxing.NotFoundException;
import com.google.zxing.RGBLuminanceSource;
import com.google.zxing.Result;
import com.google.zxing.common.HybridBinarizer;

import org.huxerui.HuxerUIFileReference;
import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

import java.io.IOException;
import java.io.InputStream;
import java.util.Collections;
import java.util.EnumMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/** Decodes camera bytes or a retained album reference on a worker thread. */
public final class QrPhotoDecoderModule implements HuxerUIPlatformModule.Factory {
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
            while (bounds.outWidth / sample > 1600 || bounds.outHeight / sample > 1600) sample *= 2;
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = sample;
            options.inPreferredConfig = Bitmap.Config.ARGB_8888;
            return options;
        }

        private static String decodeBitmap(Bitmap bitmap) {
            if (bitmap == null) return "";
            try {
                int width = bitmap.getWidth();
                int height = bitmap.getHeight();
                int[] pixels = new int[width * height];
                bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
                RGBLuminanceSource luminance =
                        new RGBLuminanceSource(width, height, pixels);
                BinaryBitmap image = new BinaryBitmap(new HybridBinarizer(luminance));
                MultiFormatReader reader = new MultiFormatReader();
                Map<DecodeHintType, Object> hints =
                        new EnumMap<>(DecodeHintType.class);
                hints.put(DecodeHintType.POSSIBLE_FORMATS,
                        Collections.singletonList(BarcodeFormat.QR_CODE));
                hints.put(DecodeHintType.TRY_HARDER, Boolean.TRUE);
                try {
                    Result result = reader.decode(image, hints);
                    return result.getText() == null ? "" : result.getText();
                } catch (NotFoundException ignored) {
                    return "";
                } finally {
                    reader.reset();
                }
            } finally {
                bitmap.recycle();
            }
        }
    }
}
