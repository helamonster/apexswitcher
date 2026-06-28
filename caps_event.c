#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>
#include <X11/Xatom.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Global variables
Display *dpy;
Window win;
KeySym original_mapping[8];
volatile sig_atomic_t running = 1; // volatile for signal handler
XftDraw *draw;
XftFont *font;
XftColor color, highlight_color;
char **desktop_names = NULL;
int num_desktops = 0;
int current_desktop = 0;
int current_viewport = 0;
int num_viewports = 1;
long current_vx = 0;
int showing = 0;
int show_names = 0;

// Atoms for EWMH properties
Atom _NET_CURRENT_DESKTOP;
Atom _NET_DESKTOP_VIEWPORT;

XineramaScreenInfo *screens = NULL;
int *desktop_to_viewport_map = NULL; // Stores the last viewport for each desktop

void cleanup() {
    printf("\nExiting. Restoring Caps Lock and cleaning up...\n");
    if (dpy) {
        if (original_mapping[0] != 0) {
            XChangeKeyboardMapping(dpy, 66, 1, original_mapping, 1);
        }
        XFlush(dpy);
        if (draw) XftDrawDestroy(draw);
        if (font) XftFontClose(dpy, font);
        if (screens) XFree(screens);
        if (desktop_to_viewport_map) free(desktop_to_viewport_map);
        if (desktop_names) {
            for (int i = 0; i < num_desktops; ++i) {
                if(desktop_names[i]) free(desktop_names[i]);
            }
            free(desktop_names);
        }
        XCloseDisplay(dpy);
    }
}

void handle_signal(int sig) {
    running = 0;
}

void grab_key_with_mods(Display *dpy, Window root, int keycode) {
    unsigned int mods[] = { 0, Mod2Mask, LockMask, Mod2Mask | LockMask };
    for (int i = 0; i < sizeof(mods)/sizeof(mods[0]); ++i) {
        XGrabKey(dpy, keycode, mods[i], root, True, GrabModeAsync, GrabModeAsync);
    }
}

void fetch_workspace_names(Display *dpy, Window root) {
    Atom net_desktop_names = XInternAtom(dpy, "_NET_DESKTOP_NAMES", True);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", True);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    if (desktop_names) {
        for (int i = 0; i < num_desktops; ++i) free(desktop_names[i]);
        free(desktop_names);
        desktop_names = NULL;
        num_desktops = 0;
    }

    XGetWindowProperty(dpy, root, net_desktop_names, 0, 4096, False, utf8,
                       &actual_type, &actual_format, &nitems, &bytes_after, &data);

    if (data && nitems > 0) {
        char *ptr = (char *)data;
        while ((unsigned char*)ptr < data + nitems) {
            desktop_names = realloc(desktop_names, (num_desktops + 1) * sizeof(char *));
            desktop_names[num_desktops++] = strdup(ptr);
            ptr += strlen(ptr) + 1;
        }
        XFree(data);
    } else {
        Atom net_number_of_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", True);
        unsigned char *ndata = NULL;
        XGetWindowProperty(dpy, root, net_number_of_desktops, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems, &bytes_after, &ndata);
        if (ndata) {
            num_desktops = *(long *)ndata;
            XFree(ndata);
        } else {
            num_desktops = 10;
        }
        desktop_names = malloc(num_desktops * sizeof(char *));
        for (int i = 0; i < num_desktops; ++i) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Desktop %d", i+1);
            desktop_names[i] = strdup(buf);
        }
    }
}

void refresh_current_state(Display *dpy, Window root) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;

    unsigned char *data = NULL;
    XGetWindowProperty(dpy, root, _NET_CURRENT_DESKTOP, 0, 1, False, XA_CARDINAL,
                       &actual_type, &actual_format, &nitems, &bytes_after, &data);
    if (data) {
        current_desktop = *(long *)data;
        XFree(data);
    }

    unsigned char *vpdata = NULL;
    XGetWindowProperty(dpy, root, _NET_DESKTOP_VIEWPORT, current_desktop * 2, 2, False, XA_CARDINAL,
                       &actual_type, &actual_format, &nitems, &bytes_after, &vpdata);

    if (vpdata && nitems >= 2) {
        long vp_x = ((long *)vpdata)[0];
        long vp_y = ((long *)vpdata)[1];
        XFree(vpdata);
        current_vx = vp_x;

        int found_viewport = 0;
        if (screens) {
            for (int i = 0; i < num_viewports; i++) {
                if (vp_x == screens[i].x_org && vp_y == screens[i].y_org) {
                    current_viewport = i;
                    found_viewport = 1;
                    break;
                }
            }
        }
        if (!found_viewport) {
            long screen_height = DisplayHeight(dpy, DefaultScreen(dpy));
            if (screen_height > 0) current_viewport = vp_y / screen_height;
        }
    }
    
    if (desktop_to_viewport_map) {
        desktop_to_viewport_map[current_desktop] = current_viewport;
    }
}

void switch_desktop(Display *dpy, Window root, int new_desktop) {
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = _NET_CURRENT_DESKTOP;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = new_desktop;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

void switch_viewport(Display *dpy, Window root, int viewport_index) {
    if (viewport_index < 0 || viewport_index >= num_viewports) return;

    long vp_x = screens ? screens[viewport_index].x_org : current_vx;
    long vp_y = screens ? screens[viewport_index].y_org : (long)viewport_index * DisplayHeight(dpy, DefaultScreen(dpy));
    printf("[switch_viewport] index=%d -> sending x=%ld y=%ld\n", viewport_index, vp_x, vp_y);

    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = _NET_DESKTOP_VIEWPORT;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = vp_x;
    ev.xclient.data.l[1] = vp_y;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

void draw_desktops() {
    XClearWindow(dpy, win);
    XWindowAttributes wa;
    XGetWindowAttributes(dpy, win, &wa);

    // Define geometry based on actual font metrics
    int font_height = font->ascent + font->descent;
    int row_padding = 10;
    int row_height = font_height + row_padding;
    int spacing = 10;
    char buf[128];

    // Calculate the width of a single line of desktop items
    int line_width = 0;
    for (int i = 0; i < num_desktops; ++i) {
        if (show_names) snprintf(buf, sizeof(buf), "[%s]", desktop_names[i]);
        else snprintf(buf, sizeof(buf), "[%02d]", i + 1);
        XGlyphInfo extents;
        XftTextExtentsUtf8(dpy, font, (XftChar8 *)buf, strlen(buf), &extents);
        line_width += extents.xOff + spacing;
    }
    line_width -= spacing;

    // Calculate the starting Y position to vertically center the block of text
    int total_content_height = (num_viewports * row_height) + ((num_viewports - 1) * spacing);
    int top_margin = (wa.height - total_content_height) / 2;

    for (int v = 0; v < num_viewports; ++v) {
        // Calculate the Y-coordinate for the baseline of the text on this row
        int y_baseline = top_margin + (v * (row_height + spacing)) + font->ascent + (row_padding / 2);
        int x_start = (wa.width - line_width) / 2;
        int x = x_start;

        for (int i = 0; i < num_desktops; ++i) {
            if (show_names) snprintf(buf, sizeof(buf), "[%s]", desktop_names[i]);
            else snprintf(buf, sizeof(buf), "[%02d]", i + 1);
            XGlyphInfo extents;
            XftTextExtentsUtf8(dpy, font, (XftChar8 *)buf, strlen(buf), &extents);
            
            XftDrawStringUtf8(draw,
                              (i == current_desktop && v == current_viewport) ? &highlight_color : &color,
                              font, x, y_baseline,
                              (XftChar8 *)buf, strlen(buf));
            x += extents.xOff + spacing;
        }
    }
}



int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "-s") == 0) show_names = 1;

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Cannot open display\n"); return 1; }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    atexit(cleanup); 

    int screen_idx = DefaultScreen(dpy);
    Window root = DefaultRootWindow(dpy);
    
    _NET_CURRENT_DESKTOP = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    _NET_DESKTOP_VIEWPORT = XInternAtom(dpy, "_NET_DESKTOP_VIEWPORT", False);

    if (XineramaIsActive(dpy)) {
        int screen_count = 0;
        screens = XineramaQueryScreens(dpy, &screen_count);
        if (screens && screen_count > 0) {
            num_viewports = screen_count;
            printf("Xinerama is active, found %d screen(s).\n", num_viewports);
        } else {
            num_viewports = 1;
            printf("Xinerama active, but failed to query screens. Assuming 1.\n");
        }
    } else {
        num_viewports = 1;
        screens = NULL;
        printf("Xinerama not active, assuming 1 screen.\n");
    }

    // If Xinerama only gave us 1 screen, check _NET_DESKTOP_GEOMETRY for virtual viewports.
    // A virtual desktop taller than the screen indicates multiple scrollable viewport rows.
    if (num_viewports == 1) {
        Atom net_desktop_geometry = XInternAtom(dpy, "_NET_DESKTOP_GEOMETRY", True);
        if (net_desktop_geometry != None) {
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *gdata = NULL;
            XGetWindowProperty(dpy, root, net_desktop_geometry, 0, 2, False, XA_CARDINAL,
                               &actual_type, &actual_format, &nitems, &bytes_after, &gdata);
            if (gdata && nitems >= 2) {
                long virt_width  = ((long *)gdata)[0];
                long virt_height = ((long *)gdata)[1];
                XFree(gdata);
                int sw = DisplayWidth(dpy, DefaultScreen(dpy));
                int sh = DisplayHeight(dpy, DefaultScreen(dpy));
                int h_vp = (sw > 0 && virt_width  > sw) ? (virt_width  / sw) : 1;
                int v_vp = (sh > 0 && virt_height > sh) ? (virt_height / sh) : 1;
                num_viewports = v_vp;  // rows only; columns are navigated via desktop switching
                // screens[] maps Xinerama physical monitors, not virtual rows — clear it so
                // switch_viewport uses virtual-row coordinates instead of screens[index].
                if (screens) { XFree(screens); screens = NULL; }
                printf("Virtual desktop geometry: %ldx%ld, screen: %dx%d -> %d viewport(s).\n",
                       virt_width, virt_height, sw, sh, num_viewports);
                printf("You have a %d-column x %d-row viewport grid (%ld/%d=%d, %ld/%d=%d).\n",
                       h_vp, v_vp, virt_width, sw, h_vp, virt_height, sh, v_vp);
            }
        }
    }
    
    memset(original_mapping, 0, sizeof(original_mapping));
    int keysyms_per_keycode;
    KeySym *orig = XGetKeyboardMapping(dpy, 66, 1, &keysyms_per_keycode);
    if (orig && keysyms_per_keycode > 0) {
        memcpy(original_mapping, orig, sizeof(KeySym) * (keysyms_per_keycode < 8 ? keysyms_per_keycode : 8));
    }
    if(orig) XFree(orig);

    KeySym new_mapping[1] = { XK_Hyper_L };
    XChangeKeyboardMapping(dpy, 66, 1, new_mapping, 1);
    XFlush(dpy);

    fetch_workspace_names(dpy, root);
    desktop_to_viewport_map = calloc(num_desktops, sizeof(int));
    refresh_current_state(dpy, root);

    font = XftFontOpenName(dpy, screen_idx, "monospace-16");
    if (!font) { fprintf(stderr, "Failed to load font: monospace-16\n"); return 1; }
    
    // FIX: Window dimensions are now calculated based on font metrics for consistency.
    int win_width, win_height, spacing = 10, line_width = 0;
    int font_height = font->ascent + font->descent;
    int row_padding = 10;
    int row_height = font_height + row_padding;
    char buf[128];
    for (int i = 0; i < num_desktops; ++i) {
        if (show_names) snprintf(buf, sizeof(buf), "[%s]", desktop_names[i]);
        else snprintf(buf, sizeof(buf), "[%02d]", i + 1);
        XGlyphInfo extents;
        XftTextExtentsUtf8(dpy, font, (XftChar8 *)buf, strlen(buf), &extents);
        line_width += extents.xOff + spacing;
    }
    line_width -= spacing;
    win_width = line_width + 60; 
    win_height = (num_viewports * row_height) + ((num_viewports - 1) * spacing) + 20;

    int win_x = (DisplayWidth(dpy, screen_idx) - win_width) / 2;
    int win_y = (DisplayHeight(dpy, screen_idx) - win_height) / 2;
    if (screens) { 
        win_x = screens[0].x_org + (screens[0].width - win_width) / 2;
        win_y = screens[0].y_org + (screens[0].height - win_height) / 2;
    }

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = WhitePixel(dpy, screen_idx);
    attrs.border_pixel = BlackPixel(dpy, screen_idx);
    win = XCreateWindow(dpy, root, win_x, win_y, win_width, win_height, 1,
                        CopyFromParent, InputOutput, DefaultVisual(dpy, screen_idx),
                        CWOverrideRedirect | CWBackPixel | CWBorderPixel, &attrs);
    XStoreName(dpy, win, "Workspace Overlay");

    draw = XftDrawCreate(dpy, win, DefaultVisual(dpy, screen_idx), DefaultColormap(dpy, screen_idx));
    XRenderColor xr_black = { 0, 0, 0, 65535 }, xr_highlight = { 35000, 35000, 60000, 65535 };
    XftColorAllocValue(dpy, DefaultVisual(dpy, screen_idx), DefaultColormap(dpy, screen_idx), &xr_black, &color);
    XftColorAllocValue(dpy, DefaultVisual(dpy, screen_idx), DefaultColormap(dpy, screen_idx), &xr_highlight, &highlight_color);

    KeyCode hyper_keycode = XKeysymToKeycode(dpy, XK_Hyper_L);
    grab_key_with_mods(dpy, root, hyper_keycode);

    XSelectInput(dpy, root, KeyPressMask | KeyReleaseMask | PropertyChangeMask);
    XSelectInput(dpy, win, ExposureMask);

    XEvent ev;
    while (running) {
        while (XPending(dpy) && running) {
            XNextEvent(dpy, &ev);
            if (ev.type == Expose && showing) {
                draw_desktops();
            } else if (ev.type == KeyPress) {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ev.xkey.keycode == hyper_keycode && !showing) {
                    refresh_current_state(dpy, root);
                    XMapRaised(dpy, win);
                    draw_desktops();
                    showing = 1;
                    XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
                } else if (showing) {
										int new_viewport = -1;
                    int new_desktop = -1;

                    switch ( ks )
										{
                        case XK_Left:
													if (current_desktop > 0)
													{
														new_desktop = current_desktop - 1;
													}
												break;

                        case XK_Right: if (current_desktop < num_desktops - 1) new_desktop = current_desktop + 1; break;
                        //case XK_Up: if (current_viewport > 0) switch_viewport(dpy, root, current_viewport - 1); break;
                        case XK_Up:

    											printf ( "\n[keypress] Up key pressed. Current viewport: %d ; Num viewports: %d\n" , current_viewport , num_viewports );
													if ( current_viewport > 0 )
													{
														new_viewport = current_viewport - 1;
    												printf ( "\nWill change viewport to %d...\n" , new_viewport );
													}
												break;

                        //case XK_Down: if (current_viewport < num_viewports - 1) switch_viewport(dpy, root, current_viewport + 1); break;
                        case XK_Down:

    											printf ( "\n[keypress] Down key pressed. Current viewport: %d ; Num viewports: %d\n" , current_viewport , num_viewports );
													if ( current_viewport < num_viewports - 1 )
													{
														new_viewport = current_viewport + 1;
    												printf ( "\nWill change viewport to %d...\n" , new_viewport );
													}
												break;

                        case XK_1: case XK_2: case XK_3: case XK_4: case XK_5:
                        case XK_6: case XK_7: case XK_8: case XK_9: case XK_0:
                        {
                            int target = (ks == XK_0) ? 9 : (int)(ks - XK_1);
                            if (target < num_desktops) new_desktop = target;
                            break;
                        }
                    }



                    if (new_desktop != -1)
										{
    									printf ( "\nChanging workspace to %d...\n" , new_desktop );
                        switch_desktop(dpy, root, new_desktop);
                        current_desktop = new_desktop;
                        draw_desktops();
                    }
										if ( new_viewport != -1)
										{
    									printf ( "\nChanging viewport to %d...\n" , new_viewport );
											switch_viewport ( dpy , root , new_viewport );
											current_viewport = new_viewport;
											draw_desktops();
										}
                }
            } else if (ev.type == KeyRelease && ev.xkey.keycode == hyper_keycode && showing) {
                XUngrabKeyboard(dpy, CurrentTime);
                XUnmapWindow(dpy, win);
                XFlush(dpy);
                showing = 0;
            } else if (ev.type == PropertyNotify && showing) {
                if (ev.xproperty.atom == _NET_CURRENT_DESKTOP || ev.xproperty.atom == _NET_DESKTOP_VIEWPORT) {
                    refresh_current_state(dpy, root);
                    draw_desktops();
                }
            }
        }
        usleep(10000);
    }
    return 0;
}

