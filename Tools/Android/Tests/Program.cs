using HEngine.AndroidHost;

int assertions = 0;
void Check(bool condition, string message)
{
    if (!condition) throw new Exception(message);
    assertions++;
}
var touch = new TouchRouter();
touch.Resize(1600, 900);
touch.Down(11, 100, 100);
touch.Down(12, 200, 200);
Check(touch.State.Buttons == 1 && touch.State.X == 100, "Only the first menu finger owns the pointer");
touch.Up(11, 110, 120);
Check(touch.State.Buttons == 0, "Releasing the primary finger must not transfer ownership");
touch.Up(12, 200, 200);
touch.EnableControls(true);
touch.Down(3, touch.StickX, touch.StickY);
touch.Move(3, touch.StickX + 1000, touch.StickY - 1000);
Check(Math.Abs(touch.State.MoveX * touch.State.MoveX + touch.State.MoveY * touch.State.MoveY - 1) < .0001,
    "Diagonal movement must be radially clamped");
touch.Down(17, touch.JumpX, touch.JumpY);
Check((touch.State.Buttons & 4) != 0 && touch.State.MoveX > 0, "Jump and stick must coexist");
touch.Up(17, 0, 0);
Check(touch.State.Buttons == 0 && touch.State.MoveX > 0, "Jump release must preserve stick ownership");
touch.Down(19, 1100, 300);
touch.Move(19, 1150, 350);
Check((touch.State.Buttons & 2) != 0 && touch.State.MoveX > 0 && touch.State.X == 1150,
    "Orbit drag must coexist with movement");
touch.Up(3, 100, 100);
Check(touch.State.MoveX == 0 && (touch.State.Buttons & 2) != 0, "Stick release must preserve camera drag");
touch.Cancel();
Check(touch.State == default, "Pause/cancel clears every control");
touch.Down(0, touch.PauseX, touch.PauseY);
Check(touch.State.Buttons == 8, "Pause control must share its drawn hit region");
touch.Resize(900, 500);
Check(touch.State == default, "Resize cancels existing pointers");
touch.Down(5, touch.StickX, touch.StickY);
touch.Move(5, touch.StickX + 1, touch.StickY);
Check(touch.State.MoveX == 0, "Stick dead zone rejects small jitter");
touch.EnableControls(false);
Check(touch.State == default, "Leaving gameplay clears held controls");
touch.Down(1, float.NaN, 0);
Check(touch.State == default, "Non-finite coordinates must be rejected");
bool rejected = false;
try { touch.Resize(0, 100); } catch (ArgumentOutOfRangeException) { rejected = true; }
Check(rejected, "Zero-sized layouts must be rejected");
Console.WriteLine($"PASS: {assertions} Android touch contract assertions");
