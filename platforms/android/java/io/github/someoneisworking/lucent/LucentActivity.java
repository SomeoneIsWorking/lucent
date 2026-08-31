package io.github.someoneisworking.lucent;

import android.os.Build;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import org.libsdl.app.SDLActivity;

/**
 * Base SDLActivity for Lucent-backed SDL3 applications.
 *
 * Owns system UI visibility (sticky immersive landscape fullscreen) and graceful task termination.
 */
public class LucentActivity extends SDLActivity {
    private final LucentTouchContacts touchContacts = new LucentTouchContacts();

    /**
     * Installs title-owned contact interpretation without taking over Android or SDL event delivery.
     *
     * <p>The listener receives physical-pixel contacts before SDL handles the same event. It must map
     * those contacts through the title's normal input policy; it must not return Android event-consumed
     * state or create a second input transport.</p>
     */
    public final void setRawTouchListener(LucentTouchContacts.Listener listener) {
        touchContacts.setListener(listener);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemUI();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUI();
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        captureTouchContacts(event);
        return super.dispatchTouchEvent(event);
    }

    @Override
    protected void onPause() {
        touchContacts.cancelAll();
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUI();
        } else {
            touchContacts.cancelAll();
        }
    }

    @Override
    protected void onDestroy() {
        touchContacts.cancelAll();
        super.onDestroy();
    }

    /**
     * Enforce sticky immersive fullscreen hiding the navigation bar and status bar.
     */
    protected void hideSystemUI() {
        Window window = getWindow();
        if (window == null) {
            return;
        }

        // PhoneWindow may not have installed its decor yet while SDLActivity is finishing
        // onCreate. Window#getInsetsController dereferences that absent decor on API 35.
        // Obtaining the decor first creates it when necessary, and its controller is only
        // available once the view is attached.
        View decor = window.getDecorView();
        if (decor == null) {
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController controller = decor.getWindowInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            decor.setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_FULLSCREEN);
        }
    }

    /**
     * Terminate the Android task and activity cleanly from native code or UI.
     */
    public void finishApp(String unused) {
        runOnUiThread(this::finishAndRemoveTask);
    }

    private void captureTouchContacts(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                captureDown(event, event.getActionIndex());
                break;
            case MotionEvent.ACTION_MOVE:
                for (int index = 0; index < event.getPointerCount(); ++index) {
                    touchContacts.update(
                            event.getPointerId(index), event.getX(index), event.getY(index),
                            event.getPressure(index));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                captureUp(event, event.getActionIndex());
                break;
            case MotionEvent.ACTION_CANCEL:
                touchContacts.cancelAll();
                break;
            default:
                break;
        }
    }

    private void captureDown(MotionEvent event, int index) {
        touchContacts.down(event.getPointerId(index), event.getX(index), event.getY(index),
                event.getPressure(index));
    }

    private void captureUp(MotionEvent event, int index) {
        touchContacts.up(event.getPointerId(index), event.getX(index), event.getY(index),
                event.getPressure(index));
    }
}
