package com.bilko.opentievr;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * OpenTIE VR launcher. The engine runs entirely in libmain.so (SDL_main); this
 * class only supplies the library list, the game-data location and the
 * storage permission the original game files need.
 */
public class OpenTIEActivity extends SDLActivity {
    private static final String TAG = "OpenTIE";

    /** Original TIE Fighter installations copied from the PC: the 1995
     *  Collector's CD-ROM and the 1998 Windows release. */
    public static final String GAME_DATA_DIR = "/sdcard/OpenTIE";
    public static final String TIE95_DATA_DIR = GAME_DATA_DIR + "/TIE95";
    public static final String TIE98_DATA_DIR = GAME_DATA_DIR + "/TIE98";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
            Log.w(TAG, "all-files access missing; asking the user to grant it");
            try {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivity(intent);
            } catch (Exception e) {
                Log.e(TAG, "could not open the all-files access settings", e);
            }
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected String[] getArguments() {
        return new String[] { "--tie95-data", TIE95_DATA_DIR, "--tie98-data", TIE98_DATA_DIR };
    }
}
