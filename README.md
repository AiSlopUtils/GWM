# GWM — Geo's Window Manager

An **infinite-canvas desktop environment** for Linux/Xorg, in a single C file.

Windows live at coordinates on an unbounded 2D canvas. A viewport (pan offset +
zoom factor) decides what you see. **Zoom is pure display scaling**: window
contents are scaled like an image by the compositor — apps never resize or
reflow when you zoom. And **zoom is permanent and fully interactive**: you can
click, type, scroll and drag windows at any zoom level, and the zoom stays
where you put it. GWM is an XRender **compositor**, which also draws the
window decorations and animates pan/zoom transitions smoothly.

## Building

```sh
sudo apt install build-essential libx11-dev libxext-dev libxrender-dev \
                 libxcomposite-dev libxdamage-dev libxfixes-dev
make
sudo make install        # installs /usr/local/bin/gwm
```

Needs the X Composite, Render, and Damage extensions (present on any stock
Xorg). Without `libxdamage-dev` it builds in poll-repaint mode using the
vendored headers in `vendor/` — fine for testing, slightly higher idle CPU.
Optional: `libimlib2-dev` at build time enables wallpaper support.

## Running

```sh
echo 'exec gwm' > ~/.xinitrc
startx
```

To pick GWM from a display manager (GDM/LightDM/SDDM) instead of startx,
install a session entry:

```sh
sudo tee /usr/share/xsessions/gwm.desktop >/dev/null <<'EOF'
[Desktop Entry]
Name=GWM
Comment=Infinite-canvas window manager
Exec=gwm
Type=Application
EOF
```

GTK/GNOME apps need a D-Bus session; display managers provide one. With
startx, use `exec dbus-launch --exit-with-session gwm` in `~/.xinitrc`.

## Keybindings

Mod = Super (the Windows key).

| Keys | Action |
|---|---|
| `Mod+Arrow keys` | pan the canvas |
| `Mod+=` / `Mod+-` | zoom in / out (out: smooth image-like scaling; in: sharp whole steps, 100→200→300→400%) |
| `Mod+scroll wheel` | zoom at the cursor |
| `Mod+0` | reset zoom to 100% |
| `Mod+R` | launcher — type a command, `Tab` completes, `Enter` runs |
| `Mod+M` | task manager — open windows with icon, name, RAM and CPU; double-click a row to jump to that window. System tray icons dock in a row at the bottom |
| `Mod+Enter` | spawn a terminal |
| `Mod+Shift+Left/Right` | snap focused window to left / right half |
| `Mod+Shift+Up` | fullscreen (toggle) |
| `Mod+Shift+Down` | restore snapped window |
| `Mod+Tab` | cycle focus (flies to the window) |
| `Mod+Q` | close window |
| `Mod+L` | lock the screen (runs the `locker` command from the config; install `i3lock`) |
| `Mod+Shift+L` | log out (ends the session, back to the login screen) |
| `Mod+Shift+E` | quit GWM |
| `Mod+Left-drag` | move window — release at the **left/right/top edge** for half / half / fullscreen, or in a **screen corner** for quarter tiling |
| `Mod+Right-drag` | resize window |

**Window decorations** (no modifier needed): every window has a title bar with
a green **fullscreen** button and a red **close** button. Drag the title bar to
move (edge snapping works there too), drag any window border or corner to
resize (the cursor changes over the resize zones), click to focus and raise.
Drag empty canvas to pan. Focus follows the mouse. All of it works at any zoom
level — zoom never resets unless you press `Mod+0`.

EWMH support includes `_NET_WM_STATE_FULLSCREEN` (browser F11 / video
fullscreen fills the viewport with no decorations), `_NET_ACTIVE_WINDOW`
(apps asking for attention fly the canvas to them), and `_NET_CLIENT_LIST`
(wmctrl and scripts work). GWM is also the XEmbed system tray manager: tray
apps (nm-applet, pavucontrol, blueman, ...) dock their icons into the bottom
row of the `Mod+M` panel and are fully clickable there.

## Configuration

GWM reads `~/.config/infinawm/config.yml` (or `~/.infinawm.yml`) at startup,
and writes a commented default on first run:

```yaml
# programs launched once at startup, separated by spaces
autostart: "nm-applet pavucontrol xterm"

# colors, hex (#rgb / #rrggbb)
background_color: "#000000"
border_color: "#ffffff"
focus_color: "#4a90e2"

# wallpaper (png/jpg). Scaled to cover the screen and screen-FIXED:
# windows pan and zoom over it, the background never moves.
background_image: "~/Pictures/wall.png"
```

Wallpaper support needs Imlib2 at build time (`sudo apt install
libimlib2-dev`, then rebuild) — without it the image setting is ignored and
the flat background color is used.

## How it works

- The WM takes `SubstructureRedirect` on the root and redirects all windows
  offscreen with `XCompositeRedirectSubwindows(..., Manual)`.
- Each frame it composites every window's backing pixmap onto a backbuffer,
  scaled by the zoom factor with `XRenderSetPictureTransform` (bilinear) —
  that's the image-like zoom. Title bars, buttons and borders are drawn
  straight into the backbuffer — clients are never reparented or resized by
  zooming.
- Input while zoomed uses a **pointer-anchored layout**: X11 can't transform
  input coordinates, so instead the real (server-side) window layout is the
  rendered layout scaled up by `1/zoom` about the cursor, re-applied as the
  pointer moves (~60 Hz). The window point under the cursor always coincides
  with the rendered point under the cursor — and input only ever happens at
  the cursor — so clicks, typing and scrolling are exact at any zoom level.
- Decoration input (title bars, resize bands) is caught by one invisible
  `InputOnly` "frame" window per client, stacked directly beneath it.
- Pan/zoom changes animate by easing the viewport toward its target each frame.
- The launcher is a tiny built-in bar (no dmenu dependency) with PATH scanning
  and Tab completion.

## Repo layout

```
src/infinawm.c      the entire WM + compositor
Makefile            auto-detects dev headers, falls back to vendor/
vendor/             minimal X11 extension headers for header-less builds
```

Known limitations: multiple monitors are treated as one large canvas viewport
rather than separate screens, and windows that set minimum size hints can
still be resized smaller (apps clamp themselves when they care).
