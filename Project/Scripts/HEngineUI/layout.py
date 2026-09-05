"""
HEngineUI.layout — Context-manager helpers for quick ImGui layout.

Examples
--------
    from HEngineUI.layout import Window, Column, Group
    import HEngine

    UI = HEngine.UI

    with Window("HUD", flags=UI.WindowFlags_NoDecoration):
        UI.Text("Score: 100")

    with Column(3):
        UI.Text("Col 1")
        UI.NextColumn()
        UI.Text("Col 2")
        UI.NextColumn()
        UI.Text("Col 3")

    with Group():
        UI.Text("Grouped content")
        UI.Button("OK")
"""

import HEngine

UI = HEngine.UI


def S(v: float) -> float:
    """Scale logical pixels to current DPI-aware UI pixels."""
    return UI.Scale(float(v))


# ---------------------------------------------------------------------------
# Window
# ---------------------------------------------------------------------------

class Window:
    """Context manager that wraps ``UI.Begin`` / ``UI.End``.

    Parameters
    ----------
    title : str
        Window title (ImGui ID).
    pos : tuple[float, float] | None
        If given, calls ``UI.SetNextWindowPos`` before ``Begin``.
    size : tuple[float, float] | None
        If given, calls ``UI.SetNextWindowSize`` before ``Begin``.
    flags : int
        ``ImGuiWindowFlags_*`` bit-mask (use ``HEngine.UI.WindowFlags_*``
        constants).
    bg_alpha : float | None
        If given, sets the window background transparency.
    """

    def __init__(self, title: str, *,
                 pos=None, size=None,
                 flags: int = 0,
                 bg_alpha: float = None):
        self._title = title
        self._pos = pos
        self._size = size
        self._flags = flags
        self._bg_alpha = bg_alpha
        self._open = False

    def __enter__(self):
        if self._pos is not None:
            vx, vy = UI.GetViewportOffset()
            UI.SetNextWindowPos(float(S(self._pos[0])) + vx, float(S(self._pos[1])) + vy)
        if self._size is not None:
            UI.SetNextWindowSizeScaled(float(self._size[0]), float(self._size[1]))
        if self._bg_alpha is not None:
            UI.SetNextWindowBgAlpha(self._bg_alpha)
        self._open = UI.Begin(self._title, self._flags)
        return self._open

    def __exit__(self, *_):
        UI.End()


# ---------------------------------------------------------------------------
# Group
# ---------------------------------------------------------------------------

class Group:
    """Context manager that wraps ``UI.BeginGroup`` / ``UI.EndGroup``."""

    def __enter__(self):
        UI.BeginGroup()
        return self

    def __exit__(self, *_):
        UI.EndGroup()


# ---------------------------------------------------------------------------
# Column
# ---------------------------------------------------------------------------

class Column:
    """Context manager that sets up ImGui columns.

    Parameters
    ----------
    count : int
        Number of columns (1 = reset to default layout).
    border : bool
        Whether to draw column borders.
    """

    def __init__(self, count: int = 1, border: bool = True):
        self._count = count
        self._border = border

    def __enter__(self):
        UI.Columns(self._count, self._border)
        return self

    def __exit__(self, *_):
        UI.Columns(1, False)


# ---------------------------------------------------------------------------
# Child
# ---------------------------------------------------------------------------

class Child:
    """Context manager that wraps ``UI.BeginChild`` / ``UI.EndChild``.

    Parameters
    ----------
    id : str
        Unique child window ID.
    width, height : float
        Size in pixels (0 = fill available).
    border : bool
        Whether to draw a border.
    """

    def __init__(self, id: str, width: float = 0.0,
                 height: float = 0.0, border: bool = False):
        self._id = id
        self._w = width
        self._h = height
        self._border = border

    def __enter__(self):
        UI.BeginChildScaled(self._id, self._w, self._h, self._border)
        return self

    def __exit__(self, *_):
        UI.EndChild()


# ---------------------------------------------------------------------------
# StyleColor
# ---------------------------------------------------------------------------

class StyleColor:
    """Context manager for ``UI.PushStyleColor`` / ``UI.PopStyleColor``.

    Parameters
    ----------
    idx : int
        Color index (``HEngine.UI.Col_*``).
    r, g, b, a : float
        RGBA color components in [0, 1].
    """

    def __init__(self, idx: int, r: float, g: float, b: float, a: float = 1.0):
        self._idx = idx
        self._r = r
        self._g = g
        self._b = b
        self._a = a

    def __enter__(self):
        UI.PushStyleColor(self._idx, self._r, self._g, self._b, self._a)
        return self

    def __exit__(self, *_):
        UI.PopStyleColor()


# ---------------------------------------------------------------------------
# StyleVar
# ---------------------------------------------------------------------------

class StyleVar:
    """Context manager for ``UI.PushStyleVar`` / ``UI.PopStyleVar``.

    Parameters
    ----------
    idx : int
        Var index (``HEngine.UI.StyleVar_*``).
    value : float or tuple[float, float]
        Scalar or (x, y) pair.
    """

    def __init__(self, idx: int, value):
        self._idx = idx
        self._value = value

    def __enter__(self):
        if isinstance(self._value, (list, tuple)):
            UI.PushStyleVar2(self._idx, float(self._value[0]), float(self._value[1]))
        else:
            UI.PushStyleVar(self._idx, float(self._value))
        return self

    def __exit__(self, *_):
        UI.PopStyleVar()
