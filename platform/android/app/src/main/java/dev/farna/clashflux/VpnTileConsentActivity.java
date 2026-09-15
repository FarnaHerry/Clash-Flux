package dev.farna.clashflux;

import android.app.Activity;
import android.content.Intent;
import android.net.VpnService;
import android.os.Bundle;

/** Hosts the one-time system VPN consent flow initiated from the QS tile. */
public final class VpnTileConsentActivity extends Activity {
    private static final int REQUEST_VPN_CONSENT = 1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent consent = VpnService.prepare(this);
        if (consent == null) {
            startVpn();
        } else {
            startActivityForResult(consent, REQUEST_VPN_CONSENT);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_VPN_CONSENT && resultCode == RESULT_OK) {
            startVpn();
        } else {
            MainActivity.appLog("用户取消了通知栏快捷开关的 VPN 授权", true);
            finish();
        }
    }

    private void startVpn() {
        try {
            VpnTileService.startVpnService(this);
        } catch (RuntimeException error) {
            MainActivity.appLog("快捷开关启动 VPN 失败：" + error.getMessage(), true);
        }
        finish();
    }
}
