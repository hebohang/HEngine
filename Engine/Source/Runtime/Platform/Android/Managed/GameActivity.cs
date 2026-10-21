using Android.App;
using Android.Content;
using Android.Content.PM;
using Android.Graphics;
using Android.OS;
using Android.Runtime;
using Android.Views;
using Android.Widget;
using Path = System.IO.Path;

namespace HEngine.AndroidHost;

[Activity(Name = "org.hengine.runtime.GameActivity", MainLauncher = true, Exported = true,
    ScreenOrientation = ScreenOrientation.SensorLandscape,
    ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.KeyboardHidden |
        ConfigChanges.ScreenLayout | ConfigChanges.SmallestScreenSize | ConfigChanges.UiMode | ConfigChanges.Density)]
public sealed class GameActivity : Activity, ISurfaceHolderCallback
{
    private TouchOverlay? mTouch;
    private bool bDestroyed;
    private bool bHostOwned;
    private BackCallback? mBackCallback;
    protected override void OnCreate(Bundle? savedInstanceState)
    {
        base.OnCreate(savedInstanceState);
        if (Native.Prepare() == 0) { ShowError("The previous game session is still closing. Please reopen the game."); return; }
        bHostOwned = true;
        if (OperatingSystem.IsAndroidVersionAtLeast(33))
        {
            mBackCallback = new BackCallback(HandleBack);
            Window!.OnBackInvokedDispatcher.RegisterOnBackInvokedCallback(0, mBackCallback);
        }
        Window!.AddFlags(WindowManagerFlags.KeepScreenOn);
        var attributes = Window.Attributes!;
        attributes.LayoutInDisplayCutoutMode = LayoutInDisplayCutoutMode.ShortEdges;
        Window.Attributes = attributes;
        var frame = new GameFrame(this);
        var surface = new SurfaceView(this);
        surface.Holder!.AddCallback(this);
        mTouch = new TouchOverlay(this);
        frame.AddView(surface); frame.AddView(mTouch);
        SetContentView(frame);
        _ = StartGame();
    }
    private async Task StartGame()
    {
        try
        {
            string files = FilesDir!.AbsolutePath;
            string content = await Task.Run(() => ContentStore.Extract(Assets!, files));
            if (bDestroyed) return;
            string saved = Path.Combine(files, "saved");
            Directory.CreateDirectory(saved);
            int result = await Task.Run(() => Native.RunGame(content, saved));
            if (!bDestroyed)
            {
                if (result == 0) Finish();
                else ShowError("The game could not start.\n" + Native.LastError());
            }
        }
        catch (Exception error) { if (!bDestroyed) ShowError(error.Message); }
    }
    private void ShowError(string message)
    {
        SetContentView(new TextView(this) { Text = message, TextSize = 20, Gravity = GravityFlags.Center });
    }
    protected override void OnResume()
    {
        base.OnResume();
        if (!bHostOwned) return;
        Native.Resume(1);
        mTouch?.ResetInput();
        if (OperatingSystem.IsAndroidVersionAtLeast(30))
        {
            if (!OperatingSystem.IsAndroidVersionAtLeast(35)) Window!.SetDecorFitsSystemWindows(false);
            var controller = Window!.InsetsController;
            if (controller != null)
            {
                controller.SystemBarsBehavior = (int)WindowInsetsControllerBehavior.ShowTransientBarsBySwipe;
                controller.Hide(WindowInsets.Type.SystemBars());
            }
        }
        else
        {
            Window!.DecorView.SystemUiFlags = SystemUiFlags.ImmersiveSticky |
                SystemUiFlags.Fullscreen | SystemUiFlags.HideNavigation | SystemUiFlags.LayoutStable |
                SystemUiFlags.LayoutFullscreen | SystemUiFlags.LayoutHideNavigation;
        }
    }
    protected override void OnPause()
    {
        mTouch?.ResetInput(); if (bHostOwned) Native.Resume(0); base.OnPause();
    }
    protected override void OnDestroy()
    {
        bDestroyed = true;
        if (OperatingSystem.IsAndroidVersionAtLeast(33) && mBackCallback != null)
        {
            Window!.OnBackInvokedDispatcher.UnregisterOnBackInvokedCallback(mBackCallback);
            mBackCallback.Dispose(); mBackCallback = null;
        }
        if (bHostOwned) Native.Stop();
        base.OnDestroy();
    }
    public void SurfaceCreated(ISurfaceHolder holder) => Native.Surface(JNIEnv.Handle, holder.Surface!.Handle);
    public void SurfaceChanged(ISurfaceHolder holder, Format format, int width, int height)
    {
        Native.Surface(JNIEnv.Handle, holder.Surface!.Handle);
        mTouch?.ResetInput();
    }
    public void SurfaceDestroyed(ISurfaceHolder holder) => Native.Surface(JNIEnv.Handle, 0);
    private sealed class BackCallback(Action action) : Java.Lang.Object, global::Android.Window.IOnBackInvokedCallback
    {
        public void OnBackInvoked() => action();
    }
    public override void OnBackPressed() => HandleBack();
    private void HandleBack()
    {
        if (Native.TouchControls() != 0)
        {
            Native.Input(0, 0, 0, 0, 8); Native.Input(0, 0, 0, 0, 0);
            return;
        }
        new AlertDialog.Builder(this)!.SetMessage("Exit game?")!
            .SetPositiveButton("Exit", (_, _) => Finish())!
            .SetNegativeButton("Cancel", (_, _) => {})!.Show();
    }
    private sealed class GameFrame(Context context) : FrameLayout(context)
    {
        protected override void OnLayout(bool changed, int left, int top, int right, int bottom)
        {
            int availableWidth = Width - PaddingLeft - PaddingRight;
            int availableHeight = Height - PaddingTop - PaddingBottom;
            // The native viewport follows the host. Aspect ratios and anchoring
            // belong to the engine UI layout, never to the Android activity.
            int width = Math.Max(availableWidth, 0);
            int height = Math.Max(availableHeight, 0);
            int x = PaddingLeft;
            int y = PaddingTop;
            for (int i = 0; i < ChildCount; ++i) GetChildAt(i)!.Layout(x, y, x + width, y + height);
        }
    }
    private sealed class TouchOverlay : View
    {
        private readonly TouchRouter mRouter = new();
        private readonly Paint mPaint = new(PaintFlags.AntiAlias);
        public TouchOverlay(Context context) : base(context) { SetWillNotDraw(false); }
        public void ResetInput() { mRouter.Cancel(); Send(); Invalidate(); }
        private void Send()
        {
            var state = mRouter.State;
            Native.Input(state.X, state.Y, state.MoveX, state.MoveY, state.Buttons);
        }
        protected override void OnSizeChanged(int w, int h, int oldw, int oldh)
        {
            base.OnSizeChanged(w, h, oldw, oldh);
            if (w > 0 && h > 0) mRouter.Resize(w, h);
            Send();
        }
        public override bool OnTouchEvent(global::Android.Views.MotionEvent? e)
        {
            if (e == null) return false;
            mRouter.EnableControls(Native.TouchControls() != 0);
            int index = e.ActionIndex;
            switch (e.ActionMasked)
            {
                case MotionEventActions.Down:
                case MotionEventActions.PointerDown: mRouter.Down(e.GetPointerId(index), e.GetX(index), e.GetY(index)); break;
                case MotionEventActions.Move:
                    for (int i = 0; i < e.PointerCount; ++i) mRouter.Move(e.GetPointerId(i), e.GetX(i), e.GetY(i));
                    break;
                case MotionEventActions.Up:
                case MotionEventActions.PointerUp: mRouter.Up(e.GetPointerId(index), e.GetX(index), e.GetY(index)); break;
                case MotionEventActions.Cancel: mRouter.Cancel(); break;
            }
            Send(); Invalidate(); return true;
        }
        protected override void OnDraw(Canvas canvas)
        {
            base.OnDraw(canvas);
            bool controls = Native.TouchControls() != 0;
            mRouter.EnableControls(controls);
            if (controls)
            {
                mPaint.Color = new Color(225, 239, 255, 95);
                float radius = mRouter.StickRadius;
                canvas.DrawCircle(mRouter.StickX, mRouter.StickY, radius, mPaint);
                mPaint.Color = new Color(225, 239, 255, 185);
                canvas.DrawCircle(mRouter.StickX + mRouter.State.MoveX * radius * .6f,
                    mRouter.StickY - mRouter.State.MoveY * radius * .6f, radius * .35f, mPaint);
                canvas.DrawCircle(mRouter.JumpX, mRouter.JumpY, radius * .8f, mPaint);
                canvas.DrawCircle(mRouter.PauseX, mRouter.PauseY, radius * .6f, mPaint);
                mPaint.Color = Color.Black; mPaint.TextAlign = Paint.Align.Center; mPaint.TextSize = radius * .32f;
                canvas.DrawText("JUMP", mRouter.JumpX, mRouter.JumpY + mPaint.TextSize * .35f, mPaint);
                canvas.DrawText("II", mRouter.PauseX, mRouter.PauseY + mPaint.TextSize * .35f, mPaint);
            }
            if (HasWindowFocus) PostInvalidateDelayed(50);
        }
    }
}
