package dev.farna.clashflux;

import android.os.Bundle;

/** Control and compact state boundary between the UI and sing-box processes. */
interface IClashRuntime {
    Bundle snapshot();
    Bundle logsAfter(long sequence);
    boolean acknowledgeTunReset();
    boolean selectOutbound(String group, String name);
    boolean urlTest(String group);
    boolean setClashMode(String mode);
    boolean refreshProxyGroups();
    boolean closeConnection(String id);
    boolean closeAllConnections();
    boolean stopRuntime();
}
