package cn.xfangfang.wiliwili;

import android.app.Activity;
import android.graphics.PixelFormat;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.os.Build;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.RelativeLayout;

/** A decoder-owned HDR layer underneath the transparent SDL controls/danmaku. */
final class VideoSurface implements SurfaceHolder.Callback {
    private final Activity activity;
    private final FrameLayout container;
    private final SurfaceView video;
    private volatile Surface surface;
    private final boolean hdr10;

    private static native void nativeSurfaceChanged();

    VideoSurface(Activity activity, ViewGroup root) {
        this.activity = activity;
        container = new FrameLayout(activity);
        container.setClipChildren(true);
        video = new SurfaceView(activity);
        video.getHolder().setFormat(PixelFormat.OPAQUE);
        video.getHolder().addCallback(this);
        video.setFocusable(false);
        container.addView(video, new FrameLayout.LayoutParams(1, 1, Gravity.CENTER));
        // A live 1x1 surface lets mpv configure MediaCodec before the first frame.
        root.addView(container, 0, new RelativeLayout.LayoutParams(1, 1));
        hdr10 = detectHdr10(activity);
    }

    private static boolean detectHdr10(Activity activity) {
        if (Build.VERSION.SDK_INT < 24) return false;
        Display display = activity.getWindowManager().getDefaultDisplay();
        boolean hdrDisplay = false;
        for (int type : display.getHdrCapabilities().getSupportedHdrTypes()) {
            if (type == Display.HdrCapabilities.HDR_TYPE_HDR10) hdrDisplay = true;
        }
        boolean hdrDecoder = false;
        for (MediaCodecInfo codec : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (codec.isEncoder()) continue;
            for (String mime : codec.getSupportedTypes()) {
                if (!mime.equalsIgnoreCase("video/hevc")) continue;
                try {
                    for (MediaCodecInfo.CodecProfileLevel profile : codec.getCapabilitiesForType(mime).profileLevels) {
                        if (profile.profile == MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10HDR10
                                || profile.profile == MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10HDR10Plus) {
                            hdrDecoder = true;
                            Log.i("wiliwili-hdr", "HDR10 decoder: " + codec.getName());
                            break;
                        }
                    }
                } catch (IllegalArgumentException error) {
                    Log.w("wiliwili-hdr", "Cannot query " + codec.getName(), error);
                }
            }
        }
        Log.i("wiliwili-hdr", "HDR10 display=" + hdrDisplay + " decoder=" + hdrDecoder);
        return hdrDisplay && hdrDecoder;
    }

    Surface acquire() { return surface; }
    boolean supportsHdr10() { return hdr10; }

    void setBounds(int x, int y, int width, int height, float aspect, boolean crop, boolean stretch) {
        activity.runOnUiThread(() -> {
            RelativeLayout.LayoutParams outer = new RelativeLayout.LayoutParams(Math.max(1, width), Math.max(1, height));
            outer.leftMargin = x;
            outer.topMargin = y;
            container.setLayoutParams(outer);
            int vw = Math.max(1, width), vh = Math.max(1, height);
            if (!stretch && aspect > 0 && width > 0 && height > 0) {
                if ((width / (float) height > aspect) != crop) vw = Math.max(1, Math.round(height * aspect));
                else vh = Math.max(1, Math.round(width / aspect));
            }
            video.setLayoutParams(new FrameLayout.LayoutParams(vw, vh, Gravity.CENTER));
        });
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        surface = holder.getSurface();
        nativeSurfaceChanged();
        Log.i("wiliwili-hdr", "Video Surface created");
    }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        surface = null;
        nativeSurfaceChanged();
        Log.i("wiliwili-hdr", "Video Surface destroyed");
    }
}
