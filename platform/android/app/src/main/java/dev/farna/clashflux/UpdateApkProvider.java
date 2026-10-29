package dev.farna.clashflux;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.List;

/** Grants the system installer read-only access to one private APK, never user profiles. */
public final class UpdateApkProvider extends ContentProvider {
    static final String MIME = "application/vnd.android.package-archive";

    static Uri uri(Context context, File file) throws IOException {
        File apk = ApkUpdatePolicy.resolve(context.getCacheDir(), file.getPath(), false);
        return new Uri.Builder().scheme("content").authority(context.getPackageName() + ".updates")
                .appendPath(apk.getParentFile().getName()).appendPath("package.apk").build();
    }

    private File resolve(Uri uri) throws FileNotFoundException {
        try {
            Context context = getContext();
            List<String> path = uri.getPathSegments();
            if (context == null || !"content".equals(uri.getScheme())
                    || !(context.getPackageName() + ".updates").equals(uri.getAuthority())
                    || uri.getQuery() != null || uri.getFragment() != null || path.size() != 2
                    || !path.get(0).matches("pending-[0-9]+-[0-9]+") || !"package.apk".equals(path.get(1)))
                throw new IOException("Invalid update APK URI");
            File file = new File(new File(new File(context.getCacheDir(), "app-updates"), path.get(0)), path.get(1));
            return ApkUpdatePolicy.resolve(context.getCacheDir(), file.getPath(), false);
        } catch (IOException error) {
            throw new FileNotFoundException(error.getMessage());
        }
    }

    @Override public boolean onCreate() { return true; }
    @Override public String getType(Uri uri) { return MIME; }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("Update APK access is read-only");
        return ParcelFileDescriptor.open(resolve(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        try {
            File apk = resolve(uri);
            String[] columns = projection == null ? new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE} : projection;
            MatrixCursor cursor = new MatrixCursor(columns, 1);
            Object[] values = new Object[columns.length];
            for (int index = 0; index < columns.length; ++index) {
                if (OpenableColumns.DISPLAY_NAME.equals(columns[index])) values[index] = "Clash-Flux-update.apk";
                else if (OpenableColumns.SIZE.equals(columns[index])) values[index] = apk.length();
            }
            cursor.addRow(values);
            return cursor;
        } catch (FileNotFoundException error) { throw new IllegalArgumentException(error); }
    }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("Read only"); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException("Read only"); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
        throw new UnsupportedOperationException("Read only");
    }
}
