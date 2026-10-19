package cn.xfangfang.wiliwili;

import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import android.util.Base64;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.KeyStore;
import java.security.cert.X509Certificate;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

import org.libsdl.app.BorealisHandler;
import org.libsdl.app.PlatformUtils;
import org.libsdl.app.SDLActivity;

public class WiliwiliActivity extends SDLActivity {
    private static native void nativeInitialize(Context context);
    private final Handler backHandler = new Handler(Looper.getMainLooper());
    private final Runnable releaseBack = () -> SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_BACK);
    private OnBackInvokedCallback backCallback;

    private void prepareTrustStore() throws GeneralSecurityException, IOException {
        TrustManagerFactory factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        factory.init((KeyStore) null);
        StringBuilder pem = new StringBuilder();
        for (TrustManager manager : factory.getTrustManagers()) {
            if (!(manager instanceof X509TrustManager)) continue;
            for (X509Certificate certificate : ((X509TrustManager) manager).getAcceptedIssuers()) {
                pem.append("-----BEGIN CERTIFICATE-----\n");
                pem.append(Base64.encodeToString(certificate.getEncoded(), Base64.NO_WRAP));
                pem.append("\n-----END CERTIFICATE-----\n");
            }
        }
        if (pem.length() == 0) throw new GeneralSecurityException("Android returned an empty trust store");
        File directory = new File(getFilesDir(), "wiliwili");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot create config directory");
        File pending = new File(directory, "cacert.pem.tmp");
        try (FileOutputStream output = new FileOutputStream(pending)) {
            output.write(pem.toString().getBytes(StandardCharsets.US_ASCII));
        }
        if (!pending.renameTo(new File(directory, "cacert.pem"))) throw new IOException("Cannot update trust store");
    }

    private void navigateBack() {
        backHandler.removeCallbacks(releaseBack);
        SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_BACK);
        // Keep the press visible to borealis' frame-based input polling.
        backHandler.postDelayed(releaseBack, 100);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "wiliwili"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        PlatformUtils.borealisHandler = new BorealisHandler();
        try {
            prepareTrustStore();
        } catch (GeneralSecurityException | IOException error) {
            Log.e("wiliwili", "Unable to prepare Android certificate bundle", error);
        }
        nativeInitialize(getApplicationContext());
        if (Build.VERSION.SDK_INT >= 33) {
            backCallback = this::navigateBack;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
    }

    @Override
    public void onBackPressed() {
        navigateBack();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) {
                navigateBack();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    protected void onDestroy() {
        backHandler.removeCallbacksAndMessages(null);
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        }
        super.onDestroy();
        // SDL2/borealis keep process-wide state which cannot be reused safely.
        System.exit(0);
    }
}
