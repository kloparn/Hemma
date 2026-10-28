/* The Hemma app's one screen (2.0): Hemma's own pages in a WebView, and - until it knows where Hemma is, or when it
 * can't reach it - a panel that looks for it (Finder) or takes an address typed. The app connects to nothing but
 * the Hemma it found, on the home network: other addresses open in the phone's browser. Everything else is
 * Hemma's pages, as in a browser: the household's rules stay in C on the server. The app adds what a WebView can't:
 *   - the lock screen and the notification's media controls for an audiobook (PlayerService), told by wire.js
 *     through window.HemmaApp.media() and calling back window.hemmaAction();
 *   - files to upload (the system's picker), downloads (Android's download manager, with the sign-in's cookie),
 *     a video's full screen, the Back key walking back through Hemma's pages. */
package io.github.kloparn.hemma;

import android.app.Activity;
import android.app.DownloadManager;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.EditorInfo;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.URLUtil;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.lang.ref.WeakReference;

public final class MainActivity extends Activity {
    static WeakReference<MainActivity> current = new WeakReference<>(null);
    private static final int PICK = 1;

    private SharedPreferences prefs;
    private final Handler main = new Handler(Looper.getMainLooper());
    private FrameLayout root;
    private WebView web;
    private ScrollView panel;
    private TextView status;
    private ProgressBar spinner;
    private LinearLayout list;
    private EditText typed;
    private Finder finder;
    private String base;     /* the Hemma in use: "http://192.168.1.50" */
    private String hemmaId;  /* its id (GET /hemma.json): found again at a new address */
    private String hemmaName;
    private int foundCount;
    private boolean looking; /* a Finder is asking around */
    private String onlyFound;
    private Net.Info onlyInfo;
    private ValueCallback<Uri[]> fileCb;
    private View fullView;
    boolean listening; /* an audiobook plays: the page keeps going with the screen off */

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        current = new WeakReference<>(this);
        prefs = getSharedPreferences("hemma", MODE_PRIVATE);
        base = prefs.getString("base", null);
        hemmaId = prefs.getString("id", null);
        hemmaName = prefs.getString("name", null);
        build();
        if (base != null) reconnect(null);
        else look(null); /* (asks for the home network first, where Android wants that: look()) */
        if (Build.VERSION.SDK_INT >= 33)
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(0, this::back);
    }

    /* ------------------------------------------------------------------ the screen */

    private boolean night() {
        return (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
    }

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private void build() {
        root = new FrameLayout(this);
        root.setBackgroundColor(night() ? Color.parseColor("#121316") : Color.parseColor("#f4f1ea")); /* Hemma's --bg */
        web = new WebView(this);
        web.setVisibility(View.INVISIBLE);
        web.setBackgroundColor(Color.TRANSPARENT);
        root.addView(web, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setupWeb();

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(48), dp(24), dp(24));
        TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 34);
        title.setTypeface(title.getTypeface(), android.graphics.Typeface.BOLD);
        box.addView(title);
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(16), 0, dp(8));
        spinner = new ProgressBar(this);
        row.addView(spinner, new LinearLayout.LayoutParams(dp(28), dp(28)));
        status = new TextView(this);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        status.setPadding(dp(12), 0, 0, 0);
        row.addView(status, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        box.addView(row);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        box.addView(list);
        Button again = new Button(this);
        again.setText(R.string.look_again);
        again.setOnClickListener(v -> lookAgain());
        box.addView(again, wrap());
        TextView or = new TextView(this);
        or.setText(R.string.or_type);
        or.setPadding(0, dp(28), 0, dp(6));
        box.addView(or);
        typed = new EditText(this);
        typed.setHint(R.string.address_hint);
        typed.setSingleLine(true);
        typed.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        typed.setImeOptions(EditorInfo.IME_ACTION_GO);
        typed.setOnEditorActionListener((v, id, ev) -> {
            openTyped();
            return true;
        });
        box.addView(typed);
        Button open = new Button(this);
        open.setText(R.string.open);
        open.setOnClickListener(v -> openTyped());
        box.addView(open, wrap());
        panel = new ScrollView(this);
        panel.addView(box);
        root.addView(panel, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);

        /* edge to edge (Android 15 on asks it): the page keeps clear of the bars and the keyboard by padding */
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            root.setOnApplyWindowInsetsListener((v, ins) -> {
                Insets s = fullView != null ? Insets.NONE
                    : ins.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.ime() | WindowInsets.Type.displayCutout());
                v.setPadding(s.left, s.top, s.right, s.bottom);
                return WindowInsets.CONSUMED;
            });
        }
    }

    private static LinearLayout.LayoutParams wrap() {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = 8;
        return p;
    }

    private void showPanel(String say, boolean busy) {
        panel.setVisibility(View.VISIBLE);
        status.setText(say);
        spinner.setVisibility(busy ? View.VISIBLE : View.GONE);
    }

    /* ------------------------------------------------------------------ finding Hemma */

    /* Android 17 on: no connection to the home network gets through without "devices on your local network" - the
       first look asks; a "no" asked again from Look again, or - once Android stops asking - its settings page */
    private static final String LOCAL_NETWORK = "android.permission.ACCESS_LOCAL_NETWORK";
    private static final int ASK_LOCAL = 2;

    private boolean mayReachHome() {
        return Build.VERSION.SDK_INT < 37 || checkSelfPermission(LOCAL_NETWORK) == PackageManager.PERMISSION_GRANTED;
    }

    private boolean asking; /* Android's question is up */

    private void askHome(boolean byUser) {
        showPanel(getString(R.string.needs_local), false);
        if (asking) return;
        if (prefs.getBoolean("asked_local", false) && !shouldShowRequestPermissionRationale(LOCAL_NETWORK)) {
            /* (Android won't ask again: its page for the app, but only when someone pressed Look again) */
            if (byUser)
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", getPackageName(), null)));
            return;
        }
        asking = true;
        prefs.edit().putBoolean("asked_local", true).apply();
        requestPermissions(new String[] {LOCAL_NETWORK}, ASK_LOCAL);
    }

    private void lookAgain() {
        if (!mayReachHome()) askHome(true);
        else look(hemmaId);
    }

    @Override public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        if (code != ASK_LOCAL) return;
        asking = false;
        if (mayReachHome()) {
            if (base != null) reconnect(null);
            else look(null);
        } else showPanel(getString(R.string.needs_local), false);
    }

    /** Looks on the Wi-Fi: the Hemma with this id is opened as soon as it answers (it moved to a new address);
     *  without one, every Hemma found is listed - and a lone one opened when the looking is done. */
    private void look(String id) {
        if (!mayReachHome()) {
            askHome(false);
            return;
        }
        if (finder != null) finder.stop();
        list.removeAllViews();
        foundCount = 0;
        onlyFound = null;
        looking = true;
        showPanel(getString(R.string.looking), true);
        finder = new Finder(this, new Finder.Listener() {
            @Override public void found(String at, Net.Info info) {
                if (id != null && id.equals(info.id)) {
                    open(at, info);
                    return;
                }
                foundCount++;
                onlyFound = at;
                onlyInfo = info;
                Button b = new Button(MainActivity.this);
                b.setAllCaps(false);
                b.setText(info.name + "  ·  " + at.replace("http://", ""));
                b.setOnClickListener(v -> open(at, info));
                list.addView(b, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
                status.setText(R.string.found_one);
            }

            @Override public void done(boolean onWifi) {
                looking = false;
                spinner.setVisibility(View.GONE);
                if (foundCount == 1 && id == null) open(onlyFound, onlyInfo);
                else if (foundCount == 0) status.setText(onWifi ? R.string.found_none : R.string.no_wifi);
            }
        });
        finder.start();
    }

    /** The Hemma it knows: asked first where it was; elsewhere on the Wi-Fi when it doesn't answer there */
    private void reconnect(String sayLost) {
        if (!mayReachHome()) {
            askHome(false);
            return;
        }
        if (sayLost != null) showPanel(sayLost, true);
        final String at = base;
        new Thread(() -> {
            Net.Info info = Finder.ask(at, 2500);
            main.post(() -> {
                if (info != null && (hemmaId == null || hemmaId.equals(info.id))) open(at, info);
                else look(hemmaId);
            });
        }).start();
    }

    private void openTyped() {
        String url = Net.typed(typed.getText().toString());
        if (url == null) {
            Toast.makeText(this, R.string.not_home, Toast.LENGTH_LONG).show();
            return;
        }
        if (!mayReachHome()) {
            askHome(true);
            return;
        }
        showPanel(getString(R.string.looking), true);
        new Thread(() -> {
            Net.Info info = Finder.ask(url, 4000);
            main.post(() -> {
                if (info != null) open(url, info);
                else showPanel(getString(R.string.not_hemma, url.replace("http://", "")), false);
            });
        }).start();
    }

    private void open(String at, Net.Info info) {
        if (finder != null) finder.stop();
        looking = false;
        boolean same = at.equals(base) && web.getUrl() != null && web.getUrl().startsWith(at + "/");
        base = at;
        hemmaId = info.id;
        hemmaName = info.name;
        prefs.edit().putString("base", at).putString("id", info.id).putString("name", info.name).apply();
        panel.setVisibility(View.GONE);
        web.setVisibility(View.VISIBLE);
        if (same) web.reload();
        else web.loadUrl(at + "/");
    }

    /* ------------------------------------------------------------------ the pages */

    private boolean ours(Uri u) {
        if (base == null || u == null) return false;
        Uri b = Uri.parse(base);
        return "http".equals(u.getScheme()) && b.getHost() != null && b.getHost().equalsIgnoreCase(u.getHost())
            && b.getPort() == u.getPort();
    }

    private void setupWeb() {
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false); /* (the next episode starts by itself, as in a browser) */
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setSupportMultipleWindows(false);
        s.setUserAgentString(s.getUserAgentString() + " HemmaApp/" + version()); /* (Hemma: "The Hemma app on Android") */
        CookieManager.getInstance().setAcceptCookie(true);
        WebView.setWebContentsDebuggingEnabled((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0);
        web.addJavascriptInterface(new Bridge(), "HemmaApp");
        web.setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest r) {
                if (ours(r.getUrl())) return false;
                try { /* not Hemma: the phone's browser */
                    startActivity(new Intent(Intent.ACTION_VIEW, r.getUrl()));
                } catch (ActivityNotFoundException ignored) {
                }
                return true;
            }

            @Override public void onReceivedError(WebView v, WebResourceRequest r, WebResourceError e) {
                if (!r.isForMainFrame()) return;
                web.setVisibility(View.INVISIBLE);
                reconnect(getString(R.string.lost, hemmaName != null ? hemmaName : "Hemma"));
            }
        });
        web.setWebChromeClient(new WebChromeClient() {
            @Override public boolean onShowFileChooser(WebView v, ValueCallback<Uri[]> cb, FileChooserParams p) {
                if (fileCb != null) fileCb.onReceiveValue(null);
                fileCb = cb;
                Intent i = new Intent(Intent.ACTION_GET_CONTENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
                if (p.getMode() == FileChooserParams.MODE_OPEN_MULTIPLE) i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
                try {
                    startActivityForResult(Intent.createChooser(i, null), PICK);
                } catch (ActivityNotFoundException e) {
                    fileCb = null;
                    return false;
                }
                return true;
            }

            @Override public void onShowCustomView(View view, CustomViewCallback cb) { /* a video's full screen */
                if (fullView != null) {
                    cb.onCustomViewHidden();
                    return;
                }
                fullView = view;
                fullView.setBackgroundColor(Color.BLACK);
                root.addView(view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
                bars(false);
            }

            @Override public void onHideCustomView() {
                if (fullView == null) return;
                root.removeView(fullView);
                fullView = null;
                bars(true);
            }
        });
        web.setDownloadListener((url, ua, disposition, mime, length) -> {
            Uri u = Uri.parse(url);
            if (!ours(u)) return;
            String name = URLUtil.guessFileName(url, disposition, mime);
            DownloadManager.Request r = new DownloadManager.Request(u);
            String cookie = CookieManager.getInstance().getCookie(url);
            if (cookie != null) r.addRequestHeader("Cookie", cookie);
            r.addRequestHeader("User-Agent", ua);
            r.setTitle(name);
            r.setMimeType(mime);
            r.setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED);
            r.setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, name);
            ((DownloadManager) getSystemService(DOWNLOAD_SERVICE)).enqueue(r);
            Toast.makeText(this, getString(R.string.downloading, name), Toast.LENGTH_SHORT).show();
        });
    }

    private void bars(boolean show) {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c == null) return;
            if (show) c.show(WindowInsets.Type.systemBars());
            else {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
            root.requestApplyInsets();
        } else {
            getWindow().getDecorView().setSystemUiVisibility(show ? 0
                : View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    @Override protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code != PICK || fileCb == null) return;
        Uri[] picked = null;
        if (result == RESULT_OK && data != null) {
            if (data.getClipData() != null) { /* several files */
                picked = new Uri[data.getClipData().getItemCount()];
                for (int i = 0; i < picked.length; i++) picked[i] = data.getClipData().getItemAt(i).getUri();
            } else if (data.getData() != null) picked = new Uri[] {data.getData()};
        }
        fileCb.onReceiveValue(picked);
        fileCb = null;
    }

    /* ------------------------------------------------------------------ Back, and the app in the background */

    private void back() {
        if (fullView != null) {
            web.evaluateJavascript("document.fullscreenElement&&document.exitFullscreen()", null);
            return;
        }
        if (panel.getVisibility() == View.VISIBLE && web.getUrl() != null && base != null) {
            panel.setVisibility(View.GONE);
            web.setVisibility(View.VISIBLE);
            return;
        }
        if (web.canGoBack()) web.goBack();
        else moveTaskToBack(true); /* (like Home: an audiobook keeps playing) */
    }

    @Override public boolean onKeyDown(int key, KeyEvent e) { /* (before Android 13) */
        if (key == KeyEvent.KEYCODE_BACK && Build.VERSION.SDK_INT < 33) {
            back();
            return true;
        }
        return super.onKeyDown(key, e);
    }

    @Override protected void onStart() {
        super.onStart();
        web.onResume();
        /* back in front with no Hemma open: look again (a locked phone gives an app in the back no network - the first
           look may have heard nothing - and the phone may be on the home Wi-Fi only now) */
        if (panel.getVisibility() == View.VISIBLE && !looking && !asking && foundCount == 0 && mayReachHome()) {
            if (base != null) reconnect(null);
            else look(null);
        }
    }

    @Override protected void onStop() {
        super.onStop();
        CookieManager.getInstance().flush();
        if (!listening) web.onPause(); /* (a video pauses when the screen goes off; an audiobook doesn't) */
    }

    @Override protected void onDestroy() {
        if (finder != null) finder.stop();
        if (PlayerService.instance != null) PlayerService.instance.finish();
        web.destroy();
        super.onDestroy();
    }

    String version() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    /** The lock screen's buttons (PlayerService) - the page's handlers (wire.js window.hemmaAction) */
    void action(String what, double arg) {
        main.post(() -> web.evaluateJavascript(
            "window.hemmaAction&&window.hemmaAction(" + quote(what) + "," + (Double.isNaN(arg) ? "undefined" : arg) + ")", null));
    }

    private static String quote(String s) {
        return "'" + s.replaceAll("[^a-z]", "") + "'";
    }

    /** window.HemmaApp: what the page tells the app (on a WebView thread: handed to the main one) */
    final class Bridge {
        @JavascriptInterface public void media(String json) {
            main.post(() -> {
                String url = web.getUrl();
                if (url == null || !ours(Uri.parse(url))) return; /* (only Hemma's pages) */
                PlayerService.update(MainActivity.this, json);
            });
        }

        @JavascriptInterface public String version() {
            return MainActivity.this.version();
        }
    }
}
