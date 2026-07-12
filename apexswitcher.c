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
#include <sys/select.h>

int                 debug              = 0;        // 1 if -d/--debug: print diagnostic messages
Display            *dpy;                          // connection to the X server
Window              win;                          // workspace overlay window
KeyCode             hyper_keycode      = 0;       // keycode of the remapped CapsLock key
KeySym              original_mapping[8];          // saved CapsLock keysyms, restored on exit
volatile sig_atomic_t running         = 1;        // cleared by signal handler to exit the loop
XftDraw            *draw;                         // Xft drawing context for the workspace overlay
XftFont            *font;                         // font used for all text rendering
XftColor            color;                        // normal text color (black)
XftColor            highlight_color;              // highlighted item color (blue)
XftColor            grey_color;                   // dimmed text color for placeholder messages
GC                  active_gc          = NULL;    // GC used to fill the active viewport cell background
GC                  inactive_gc        = NULL;    // GC used to fill inactive viewport cells
char              **desktop_names      = NULL;    // array of desktop name strings
int                 num_desktops       = 0;       // total number of desktops
int                 current_desktop    = 0;       // index of the currently active desktop
int                 current_viewport   = 0;       // current viewport row index
int                 num_viewports      = 1;       // number of viewport rows
long                current_vx         = 0;       // current viewport X offset (preserves column on row switch)
int                 showing            = 0;       // 1 while the workspace overlay is visible
int                 show_names         = 0;       // 1 if -n/--names: show desktop names instead of numbers

Atom _NET_CURRENT_DESKTOP;                       // atom: read/set the active desktop
Atom _NET_DESKTOP_VIEWPORT;                      // atom: read/set the viewport scroll position
Atom _NET_CLIENT_LIST;                           // atom: list of managed client windows
Atom _NET_WM_DESKTOP;                            // atom: which desktop a window belongs to
Atom _NET_WM_NAME;                               // atom: UTF-8 window title
Atom _NET_ACTIVE_WINDOW;                         // atom: send a window-activation request
Atom _NET_DESKTOP_NAMES;                         // atom: list of desktop name strings
Atom _NET_NUMBER_OF_DESKTOPS;                    // atom: total number of desktops
Atom _NET_DESKTOP_GEOMETRY;                      // atom: virtual desktop pixel dimensions
Atom UTF8_STRING;                                // atom: UTF-8 string property type

Window              win2               = 0;       // window switcher overlay window
XftDraw            *draw2              = NULL;    // Xft drawing context for the window switcher
Window             *window_ids         = NULL;    // Window IDs on the current desktop/viewport
char              **window_names       = NULL;    // title strings matching window_ids[]
int                 num_windows        = 0;       // number of entries in window_ids/window_names
int                 current_window_idx = 0;       // index of the highlighted window in the switcher
int                 showing_windows    = 0;       // 1 while the window switcher overlay is visible
int                 win_row_height     = 0;       // pixel height of one row in the window switcher list

XineramaScreenInfo *screens                 = NULL; // Xinerama screen list; NULL in virtual-viewport mode
int                *desktop_to_viewport_map = NULL; // last known viewport row for each desktop


////////////////////////////////////////////////////////////////////////////////


// Restores the original CapsLock key mapping and frees all allocated resources.
void cleanup ()
{

	printf ( "\nExiting. Restoring Caps Lock and cleaning up...\n" );

	if ( dpy )
	{
		// Restore the original CapsLock key mapping.
		if ( original_mapping[0] != 0 && hyper_keycode != 0 )
		{
			XChangeKeyboardMapping ( dpy , hyper_keycode , 1 , original_mapping , 1 );
		}
		XFlush ( dpy );

		// Free Xft drawing resources.
		if ( draw )     XftDrawDestroy ( draw );
		if ( draw2 )    XftDrawDestroy ( draw2 );
		if ( font )     XftFontClose ( dpy , font );
		if ( active_gc   ) XFreeGC ( dpy , active_gc   );
		if ( inactive_gc ) XFreeGC ( dpy , inactive_gc );

		// Free the window list.
		if ( window_ids ) free ( window_ids );
		if ( window_names )
		{
			for ( int i = 0 ; i < num_windows ; ++ i )
			{
				free ( window_names[i] );
			}
			free ( window_names );
		}

		// Free Xinerama and viewport-tracking data.
		if ( screens )                 XFree ( screens );
		if ( desktop_to_viewport_map ) free ( desktop_to_viewport_map );

		// Free desktop name strings.
		if ( desktop_names )
		{
			for ( int i = 0 ; i < num_desktops ; ++ i )
			{
				if ( desktop_names[i] ) free ( desktop_names[i] );
			}
			free ( desktop_names );
		}

		XCloseDisplay ( dpy );
	}
}


////////////////////////////////////////////////////////////////////////////////


// Signal handler: clears running to break the event loop cleanly.
void handle_signal ( int sig )
{

	(void) sig;
	running = 0;
}


////////////////////////////////////////////////////////////////////////////////


// Passively grabs a key with all common modifier combinations (NumLock, CapsLock, both).
void grab_key_with_mods ( int keycode )
{

	Window       root = DefaultRootWindow ( dpy );                              // root window
	// Cover NumLock (Mod2) and CapsLock (Lock) in every combination so the grab
	// fires regardless of whether those modifiers happen to be active.
	unsigned int mods[] = { 0 , Mod2Mask , LockMask , Mod2Mask | LockMask }; // modifier variants to cover

	for ( int i = 0 ; i < ( int ) ( sizeof ( mods ) / sizeof ( mods[0] ) ) ; ++ i )
	{
		XGrabKey ( dpy , keycode , mods[i] , root , True , GrabModeAsync , GrabModeAsync );
	}
}


////////////////////////////////////////////////////////////////////////////////


// Reads _NET_DESKTOP_NAMES (or falls back to numbered names) into desktop_names[].
void fetch_workspace_names ( void )
{

	Window        root = DefaultRootWindow ( dpy ); // root window
	Atom          actual_type;   // actual property type returned by XGetWindowProperty
	int           actual_format; // bit width of the returned property data
	unsigned long nitems;        // number of items returned
	unsigned long bytes_after;   // remaining unread bytes (unused)
	unsigned char *data = NULL;  // raw property data

	// Free any previously loaded names before refetching.
	if ( desktop_names )
	{
		for ( int i = 0 ; i < num_desktops ; ++ i )
		{
			free ( desktop_names[i] );
		}
		free ( desktop_names );
		desktop_names = NULL;
		num_desktops  = 0;
	}

	// Try to read desktop names from the root window property.
	XGetWindowProperty ( dpy , root , _NET_DESKTOP_NAMES , 0 , 4096 , False , UTF8_STRING ,
	                     & actual_type , & actual_format , & nitems , & bytes_after , & data );

	if ( data && nitems > 0 )
	{
		// Parse the null-separated UTF-8 string list into desktop_names[].
		char *ptr = ( char * ) data; // cursor through the null-separated property data

		while ( ( unsigned char * ) ptr < data + nitems )
		{
			char **tmp = realloc ( desktop_names , ( num_desktops + 1 ) * sizeof ( char * ) );
			if ( ! tmp ) { fprintf ( stderr , "apexswitcher: out of memory\n" ); exit ( 1 ); }
			desktop_names = tmp;
			desktop_names[num_desktops] = strdup ( ptr );
			if ( ! desktop_names[num_desktops] ) { fprintf ( stderr , "apexswitcher: out of memory\n" ); exit ( 1 ); }
			num_desktops ++;
			ptr += strlen ( ptr ) + 1;
		}
		XFree ( data );
	}
	else
	{
		if ( data ) XFree ( data );

		// Fall back to _NET_NUMBER_OF_DESKTOPS and generate "Desktop N" names.
		unsigned char *ndata = NULL; // raw desktop-count property data

		XGetWindowProperty ( dpy , root , _NET_NUMBER_OF_DESKTOPS , 0 , 1 , False , XA_CARDINAL ,
		                     & actual_type , & actual_format , & nitems , & bytes_after , & ndata );

		if ( ndata )
		{
			num_desktops = * ( long * ) ndata;
			XFree ( ndata );
		}
		else
		{
			num_desktops = 10; // reasonable default when the property is absent
		}

		// Generate numbered placeholder names.
		desktop_names = malloc ( num_desktops * sizeof ( char * ) );
		if ( ! desktop_names ) { fprintf ( stderr , "apexswitcher: out of memory\n" ); exit ( 1 ); }

		for ( int i = 0 ; i < num_desktops ; ++ i )
		{
			char buf[32]; // scratch buffer for the generated name
			snprintf ( buf , sizeof ( buf ) , "Desktop %d" , i + 1 );
			desktop_names[i] = strdup ( buf );
			if ( ! desktop_names[i] ) { fprintf ( stderr , "apexswitcher: out of memory\n" ); exit ( 1 ); }
		}
	}
}


////////////////////////////////////////////////////////////////////////////////


// Reads _NET_CURRENT_DESKTOP and _NET_DESKTOP_VIEWPORT into the global state variables.
void refresh_current_state ( void )
{

	Window        root = DefaultRootWindow ( dpy ); // root window
	Atom          actual_type;   // actual property type returned by XGetWindowProperty
	int           actual_format; // bit width of the returned property data
	unsigned long nitems;        // number of items returned
	unsigned long bytes_after;   // remaining unread bytes (unused)

	// Read the currently active desktop index.
	unsigned char *data = NULL; // raw _NET_CURRENT_DESKTOP property data
	XGetWindowProperty ( dpy , root , _NET_CURRENT_DESKTOP , 0 , 1 , False , XA_CARDINAL ,
	                     & actual_type , & actual_format , & nitems , & bytes_after , & data );
	if ( data )
	{
		current_desktop = * ( long * ) data;
		XFree ( data );
	}

	// Read the full _NET_DESKTOP_VIEWPORT array (one x,y pair per desktop).
	unsigned char *vpdata = NULL; // raw _NET_DESKTOP_VIEWPORT property data
	XGetWindowProperty ( dpy , root , _NET_DESKTOP_VIEWPORT , 0 , num_desktops * 2 , False , XA_CARDINAL ,
	                     & actual_type , & actual_format , & nitems , & bytes_after , & vpdata );

	if ( vpdata )
	{
		if ( nitems >= 2 )
		{
			long screen_height = DisplayHeight ( dpy , DefaultScreen ( dpy ) ); // physical screen height in pixels

			// Map every desktop to its current viewport index.
			for ( int d = 0 ; d < num_desktops && ( unsigned long ) d * 2 + 1 < nitems ; ++ d )
			{
				long vp_x = ( ( long * ) vpdata )[ d * 2 ];     // X offset for desktop d
				long vp_y = ( ( long * ) vpdata )[ d * 2 + 1 ]; // Y offset for desktop d

				int found_viewport = 0; // set to 1 once the matching viewport row is identified

				// Xinerama mode: match offset against physical screen origins.
				if ( screens )
				{
					for ( int s = 0 ; s < num_viewports ; ++ s )
					{
						if ( vp_x == screens[s].x_org && vp_y == screens[s].y_org )
						{
							if ( desktop_to_viewport_map ) desktop_to_viewport_map[d] = s;
							found_viewport = 1;
							break;
						}
					}
				}

				// Virtual viewport mode: derive index from Y offset.
				if ( ! found_viewport && screen_height > 0 )
				{
					if ( desktop_to_viewport_map ) desktop_to_viewport_map[d] = vp_y / screen_height;
				}

				// Keep current_viewport and current_vx in sync for the active desktop.
				if ( d == current_desktop )
				{
					current_vx       = vp_x;
					current_viewport = desktop_to_viewport_map ? desktop_to_viewport_map[d] : 0;
				}
			}
		}

		XFree ( vpdata );
	}
}


////////////////////////////////////////////////////////////////////////////////


// Sends a _NET_CURRENT_DESKTOP ClientMessage to switch to the given workspace.
void switch_desktop ( int new_desktop )
{

	Window root   = DefaultRootWindow ( dpy ); // root window
	XEvent ev     = {0};                        // event to send to the window manager

	ev.xclient.type         = ClientMessage;
	ev.xclient.window       = root;
	ev.xclient.message_type = _NET_CURRENT_DESKTOP;
	ev.xclient.format       = 32;
	ev.xclient.data.l[0]    = new_desktop;
	ev.xclient.data.l[1]    = CurrentTime;

	XSendEvent ( dpy , root , False , SubstructureRedirectMask | SubstructureNotifyMask , & ev );
	XFlush ( dpy );
}


////////////////////////////////////////////////////////////////////////////////


// Sends a _NET_DESKTOP_VIEWPORT ClientMessage to scroll to the given viewport row.
void switch_viewport ( int viewport_index )
{

	if ( viewport_index < 0 || viewport_index >= num_viewports )
	{
		return;
	}

	Window root = DefaultRootWindow ( dpy ); // root window

	// Compute target coordinates: preserve the horizontal column, change the row.
	long vp_x = screens ? screens[viewport_index].x_org : current_vx;                                                              // target X offset in pixels
	long vp_y = screens ? screens[viewport_index].y_org : ( long ) viewport_index * DisplayHeight ( dpy , DefaultScreen ( dpy ) ); // target Y offset in pixels

	if ( debug ) printf ( "[switch_viewport] index=%d -> sending x=%ld y=%ld\n" , viewport_index , vp_x , vp_y );

	// Send the scroll request to the window manager.
	XEvent ev = {0}; // event to send to the window manager

	ev.xclient.type         = ClientMessage;
	ev.xclient.window       = root;
	ev.xclient.message_type = _NET_DESKTOP_VIEWPORT;
	ev.xclient.format       = 32;
	ev.xclient.data.l[0]    = vp_x;
	ev.xclient.data.l[1]    = vp_y;

	XSendEvent ( dpy , root , False , SubstructureRedirectMask | SubstructureNotifyMask , & ev );
	XFlush ( dpy );
}


////////////////////////////////////////////////////////////////////////////////


// Returns the Xinerama screen index whose rectangle contains the active window's centre.
// Falls back to screen 0 (or the only logical screen) if nothing better is found.
int screen_for_active_window ( void )
{

	Window root = DefaultRootWindow ( dpy ); // root window

	// Read the active window from the WM.
	Atom          actual_type;   // property type returned
	int           actual_format; // bit width of returned data
	unsigned long nitems;        // items returned
	unsigned long bytes_after;   // unread bytes (unused)
	unsigned char *aw_data = NULL; // raw _NET_ACTIVE_WINDOW data

	XGetWindowProperty ( dpy , root , _NET_ACTIVE_WINDOW , 0 , 1 , False , XA_WINDOW ,
	                     & actual_type , & actual_format , & nitems , & bytes_after , & aw_data );

	if ( ! aw_data )
	{
		return 0;
	}

	Window active = * ( Window * ) aw_data; // the currently focused window
	XFree ( aw_data );

	if ( ! active )
	{
		return 0;
	}

	// Translate the window's top-left corner to root coordinates, then find its centre.
	int    wx;    // window X in root coordinates
	int    wy;    // window Y in root coordinates
	Window dummy; // child window at the translated point (unused)
	XTranslateCoordinates ( dpy , active , root , 0 , 0 , & wx , & wy , & dummy );

	XWindowAttributes wa; // window geometry
	if ( ! XGetWindowAttributes ( dpy , active , & wa ) ) return 0;

	int cx = wx + wa.width  / 2; // centre X of the active window
	int cy = wy + wa.height / 2; // centre Y of the active window

	// Find the Xinerama screen whose rectangle contains the centre point.
	if ( screens )
	{
		for ( int i = 0 ; i < num_viewports ; ++ i )
		{
			if ( cx >= screens[i].x_org && cx < screens[i].x_org + screens[i].width &&
			     cy >= screens[i].y_org && cy < screens[i].y_org + screens[i].height )
			{
				return i;
			}
		}
	}

	return 0;
}


////////////////////////////////////////////////////////////////////////////////


// Populates window_ids[] and window_names[] with windows on the current desktop/viewport.
void fetch_windows ()
{

	// Free previously fetched window data.
	if ( window_names )
	{
		for ( int i = 0 ; i < num_windows ; ++ i )
		{
			free ( window_names[i] );
		}
		free ( window_names );
		window_names = NULL;
	}

	if ( window_ids )
	{
		free ( window_ids );
		window_ids = NULL;
	}

	num_windows        = 0; // reset window count
	current_window_idx = 0; // reset selection to first entry

	Window        root         = DefaultRootWindow ( dpy ); // root window
	Atom          actual_type;                               // actual property type returned
	int           actual_format;                             // bit width of the returned property data
	unsigned long nitems;                                    // number of items returned
	unsigned long bytes_after;                               // remaining unread bytes (unused)

	// Read the currently focused window so it can be pre-selected in the list.
	unsigned char *aw_data = NULL;                                         // raw _NET_ACTIVE_WINDOW data
	XGetWindowProperty ( dpy , root , _NET_ACTIVE_WINDOW , 0 , 1 , False , XA_WINDOW ,
	                     & actual_type , & actual_format , & nitems , & bytes_after , & aw_data );
	Window active_win = aw_data ? * ( Window * ) aw_data : 0; // ID of the currently focused window
	if ( aw_data ) XFree ( aw_data );

	// Read the full list of managed client windows.
	unsigned char *list_data = NULL; // raw _NET_CLIENT_LIST property data
	XGetWindowProperty ( dpy , root , _NET_CLIENT_LIST , 0 , 1024 , False , XA_WINDOW ,
	                     & actual_type , & actual_format , & nitems , & bytes_after , & list_data );
	if ( ! list_data )
	{
		return;
	}

	Window        *all_wins      = ( Window * ) list_data;                       // array of all managed window IDs
	unsigned long  total_windows = nitems;                                        // total window count (saved before inner calls clobber nitems)
	int            sh            = DisplayHeight ( dpy , DefaultScreen ( dpy ) ); // physical screen height for viewport overlap check

	for ( unsigned long i = 0 ; i < total_windows ; ++ i )
	{
		Window w = all_wins[i]; // window ID being examined

		// Skip windows not on the current desktop (0xFFFFFFFF means sticky / all desktops).
		unsigned char *desk_data = NULL; // raw _NET_WM_DESKTOP property data
		XGetWindowProperty ( dpy , w , _NET_WM_DESKTOP , 0 , 1 , False , XA_CARDINAL ,
		                     & actual_type , & actual_format , & nitems , & bytes_after , & desk_data );
		if ( ! desk_data )
		{
			continue;
		}
		long win_desk = * ( long * ) desk_data; // desktop index this window is assigned to
		XFree ( desk_data );
		if ( win_desk != current_desktop && win_desk != 0xFFFFFFFFL )
		{
			continue;
		}

		// Filter by viewport: the WM moves windows into screen coordinates when scrolling,
		// so a window is in the current viewport if any part of it overlaps [0, sh).
		if ( sh > 0 )
		{
			int    wx;     // window X position in root (screen) coordinates
			int    wy;     // window Y position in root (screen) coordinates
			Window dummy;  // child window at translated point (unused)
			XTranslateCoordinates ( dpy , w , root , 0 , 0 , & wx , & wy , & dummy );
			XWindowAttributes wa;               // window geometry and state
			if ( ! XGetWindowAttributes ( dpy , w , & wa ) ) continue;
			int win_bottom = wy + ( int ) wa.height; // Y coordinate of the window's bottom edge
			if ( win_bottom <= 0 || wy >= sh )
			{
				continue;
			}
		}

		// Get the window title, preferring UTF-8 _NET_WM_NAME over the legacy XFetchName.
		char          *title     = NULL; // heap-allocated window title string
		unsigned char *name_data = NULL; // raw _NET_WM_NAME property data
		XGetWindowProperty ( dpy , w , _NET_WM_NAME , 0 , 256 , False , UTF8_STRING ,
		                     & actual_type , & actual_format , & nitems , & bytes_after , & name_data );
		if ( name_data )
		{
			title = strdup ( ( char * ) name_data );
			XFree ( name_data );
		}
		else
		{
			char *xname = NULL; // ICCCM WM_NAME string (Latin-1 fallback)
			if ( XFetchName ( dpy , w , & xname ) && xname )
			{
				title = strdup ( xname );
				XFree ( xname );
			}
			else
			{
				title = strdup ( "(untitled)" );
			}
		}
		if ( ! title ) { fprintf ( stderr , "apexswitcher: out of memory\n" ); exit ( 1 ); }

		// Append this window to the output lists.
		Window *new_ids = realloc ( window_ids , ( num_windows + 1 ) * sizeof ( Window ) );
		if ( ! new_ids ) { fprintf ( stderr , "apexswitcher: out of memory\n" ); exit ( 1 ); }
		window_ids = new_ids;
		char **new_names = realloc ( window_names , ( num_windows + 1 ) * sizeof ( char * ) );
		if ( ! new_names ) { fprintf ( stderr , "apexswitcher: out of memory\n" ); exit ( 1 ); }
		window_names = new_names;
		window_ids[num_windows]   = w;
		window_names[num_windows] = title;
		if ( w == active_win )
		{
			current_window_idx = num_windows; // pre-select the currently active window
		}
		num_windows ++;
	}

	XFree ( list_data );
}


////////////////////////////////////////////////////////////////////////////////


// Sends a _NET_ACTIVE_WINDOW ClientMessage to raise and focus the given window.
void activate_window_by_id ( Window w )
{

	Window root = DefaultRootWindow ( dpy ); // root window

	XEvent ev = {0}; // event to send to the window manager

	ev.xclient.type         = ClientMessage;
	ev.xclient.window       = w;
	ev.xclient.message_type = _NET_ACTIVE_WINDOW;
	ev.xclient.format       = 32;
	ev.xclient.data.l[0]    = 2;           // source indication: 2 = pager/switcher
	ev.xclient.data.l[1]    = CurrentTime;

	XSendEvent ( dpy , root , False , SubstructureRedirectMask | SubstructureNotifyMask , & ev );
	XFlush ( dpy );
}


////////////////////////////////////////////////////////////////////////////////


// Formats the label prefix for window at index i into buf.
// Mapping: 1-9 (number keys 1-9), 0 (key 0), F01-F12 (function keys F1-F12), blank (no binding).
static void fmt_window_label ( char *buf , size_t bufsz , int i , const char *title )
{

	if ( i < 9 )
	{
		snprintf ( buf , bufsz , "  %d. %s" , i + 1 , title );
	}
	else
	{
		if ( i == 9 )
		{
			snprintf ( buf , bufsz , "  0. %s" , title );
		}
		else
		{
			if ( i <= 21 )
			{
				snprintf ( buf , bufsz , "F%02d. %s" , i - 9 , title );
			}
			else
			{
				snprintf ( buf , bufsz , "     %s" , title );
			}
		}
	}
}


////////////////////////////////////////////////////////////////////////////////


// Redraws the window switcher overlay (win2) with the current window list.
void draw_windows ()
{

	XClearWindow ( dpy , win2 );

	int  row_height = win_row_height; // pixel height of one list row
	int  x_margin   = 15;                // left margin and initial top offset in pixels
	char buf[512];                        // formatted label + title buffer

	// Show a placeholder message when there are no windows to list.
	if ( num_windows == 0 )
	{
		const char *msg = "(no windows in current workspace)"; // placeholder text
		int         y   = x_margin + font->ascent + 4;          // text baseline Y coordinate
		XftDrawStringUtf8 ( draw2 , & grey_color , font , x_margin , y ,
		                    ( XftChar8 * ) msg , strlen ( msg ) );
		XFlush ( dpy );
		return;
	}

	// Draw each window entry, highlighting the currently selected one.
	for ( int i = 0 ; i < num_windows ; ++ i )
	{
		fmt_window_label ( buf , sizeof ( buf ) , i , window_names[i] );
		int       y   = x_margin + i * row_height + font->ascent + 4;             // text baseline Y for this row
		XftColor *clr = ( i == current_window_idx ) ? & highlight_color : & color; // per-row color
		XftDrawStringUtf8 ( draw2 , clr , font , x_margin , y ,
		                    ( XftChar8 * ) buf , strlen ( buf ) );
	}

	XFlush ( dpy );
}


////////////////////////////////////////////////////////////////////////////////


// Redraws the workspace overlay (win) showing desktops across all viewport rows.
void draw_desktops ()
{

	XClearWindow ( dpy , win );

	XWindowAttributes wa;                           // overlay window geometry
	if ( ! XGetWindowAttributes ( dpy , win , & wa ) ) return;

	int  font_height = font->ascent + font->descent; // full font height in pixels
	int  row_padding = 10;                            // vertical padding within each row
	int  row_height  = font_height + row_padding;     // total pixel height of one row
	int  spacing     = 10;                            // pixel gap between viewport rows
	char buf[128];                                    // desktop label buffer

	// Compute the pixel width of one row of desktop labels.
	int line_width = 0; // accumulated width of the label row in pixels
	for ( int i = 0 ; i < num_desktops ; ++ i )
	{
		if ( show_names )
		{
			snprintf ( buf , sizeof ( buf ) , "[%s]" , desktop_names[i] );
		}
		else
		{
			snprintf ( buf , sizeof ( buf ) , "[%02d]" , i + 1 );
		}
		XGlyphInfo extents; // glyph metrics for this label
		XftTextExtentsUtf8 ( dpy , font , ( XftChar8 * ) buf , strlen ( buf ) , & extents );
		line_width += extents.xOff + spacing;
	}
	line_width -= spacing; // remove the trailing gap

	// Compute vertical centering of the row block.
	int total_content_height = ( num_viewports * row_height ) + ( ( num_viewports - 1 ) * spacing ); // total pixel height of all rows
	int top_margin           = ( wa.height - total_content_height ) / 2;                              // pixel offset to the first row

	// Draw one row of labels per viewport row.
	for ( int v = 0 ; v < num_viewports ; ++ v )
	{
		int y_baseline = top_margin + ( v * ( row_height + spacing ) ) + font->ascent + ( row_padding / 2 ); // text baseline for this row
		int x          = ( wa.width - line_width ) / 2;                                                       // X start for centered row

		for ( int i = 0 ; i < num_desktops ; ++ i )
		{
			if ( show_names )
			{
				snprintf ( buf , sizeof ( buf ) , "[%s]" , desktop_names[i] );
			}
			else
			{
				snprintf ( buf , sizeof ( buf ) , "[%02d]" , i + 1 );
			}
			XGlyphInfo extents; // glyph metrics for this label
			XftTextExtentsUtf8 ( dpy , font , ( XftChar8 * ) buf , strlen ( buf ) , & extents );

			// Highlight the active viewport row for each desktop column independently.
			int active    = ( i == current_desktop && v == current_viewport ); // 1 if this is the current cell
			int vp_active = ( v == desktop_to_viewport_map[i] );               // 1 if this row is that desktop's viewport
			XftColor *clr = active ? & highlight_color : & color;              // text color based on active state

			// Fill yellow only on the active-viewport row for this desktop; full if current, dim if not.
			if ( vp_active )
			{
				XFillRectangle ( dpy , win , active ? active_gc : inactive_gc ,
				                 x - 2 ,
				                 y_baseline - font->ascent - ( row_padding / 2 ) ,
				                 extents.xOff + 4 ,
				                 row_height );
			}

			XftDrawStringUtf8 ( draw , clr , font , x , y_baseline ,
			                    ( XftChar8 * ) buf , strlen ( buf ) );
			x += extents.xOff + spacing;
		}
	}

	XFlush ( dpy );
}


////////////////////////////////////////////////////////////////////////////////


// Attempts XGrabKeyboard; prints the specific reason to stderr on failure.
// Returns GrabSuccess on success, or the X grab error code on failure.
static int try_grab_keyboard ( void )
{

	int result = XGrabKeyboard ( dpy , DefaultRootWindow ( dpy ) , True , // grab result code
	                             GrabModeAsync , GrabModeAsync , CurrentTime );

	if ( result != GrabSuccess )
	{
		// Map the error code to a human-readable description.
		const char *reason =
			result == AlreadyGrabbed  ? "keyboard already grabbed" :
			result == GrabInvalidTime ? "invalid grab time"        :
			result == GrabFrozen      ? "keyboard is frozen"       :
			                            "unknown error";             // fallback for unexpected codes
		fprintf ( stderr , "XGrabKeyboard failed: %s\n" , reason );
	}

	return result;
}


////////////////////////////////////////////////////////////////////////////////


// Prints usage information to the given stream.
void print_usage ( FILE *stream , const char *argv0 )
{

	fprintf ( stream ,
	    "Usage: %s [options]\n"
	    "\n"
	    "Options:\n"
	    "  -n, --names   Show desktop names instead of numbers in the workspace overlay\n"
	    "  -d, --debug   Print diagnostic messages to stdout\n"
	    "  -h, --help    Show this help text and exit\n"
	    "\n"
	    "Controls (hold CapsLock to open the workspace overlay):\n"
	    "  Left / Right  Switch workspace\n"
	    "  Up / Down     Switch viewport\n"
	    "  1-0           Jump directly to workspace 1-10\n"
	    "  Shift+Caps    Open the window switcher\n"
	    "  Up / Down     Navigate window list\n"
	    "  1-0, F1-F12   Jump directly to window by index\n"
	    , argv0 );
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////


// Initializes the display, remaps CapsLock, creates overlay windows, and runs the event loop.
int main ( int argc , char *argv[] )
{

	// Parse command-line flags.
	for ( int i = 1 ; i < argc ; ++ i )
	{
		if ( strcmp ( argv[i] , "-n" ) == 0 || strcmp ( argv[i] , "--names" ) == 0 )
		{
			show_names = 1;
		}
		else
		{
			if ( strcmp ( argv[i] , "-d" ) == 0 || strcmp ( argv[i] , "--debug" ) == 0 )
			{
				debug = 1;
			}
			else
			{
				if ( strcmp ( argv[i] , "-h" ) == 0 || strcmp ( argv[i] , "--help" ) == 0 )
				{
					print_usage ( stdout , argv[0] );
					return 0;
				}
				else
				{
					fprintf ( stderr , "%s: unknown option '%s'\n" , argv[0] , argv[i] );
					fprintf ( stderr , "Run '%s --help' for usage.\n" , argv[0] );
					return 1;
				}
			}
		}
	}

	// Open the connection to the X display.
	dpy = XOpenDisplay ( NULL );
	if ( ! dpy )
	{
		fprintf ( stderr , "Cannot open display\n" );
		return 1;
	}

	signal ( SIGINT  , handle_signal );
	signal ( SIGTERM , handle_signal );
	atexit ( cleanup );

	int    screen_idx = DefaultScreen    ( dpy ); // default screen index
	Window root       = DefaultRootWindow ( dpy ); // root window of the default screen

	// Intern all EWMH atoms used throughout the program.
	_NET_CURRENT_DESKTOP     = XInternAtom ( dpy , "_NET_CURRENT_DESKTOP"    , False );
	_NET_DESKTOP_VIEWPORT    = XInternAtom ( dpy , "_NET_DESKTOP_VIEWPORT"   , False );
	_NET_CLIENT_LIST         = XInternAtom ( dpy , "_NET_CLIENT_LIST"        , False );
	_NET_WM_DESKTOP          = XInternAtom ( dpy , "_NET_WM_DESKTOP"         , False );
	_NET_WM_NAME             = XInternAtom ( dpy , "_NET_WM_NAME"            , False );
	_NET_ACTIVE_WINDOW       = XInternAtom ( dpy , "_NET_ACTIVE_WINDOW"      , False );
	_NET_DESKTOP_NAMES       = XInternAtom ( dpy , "_NET_DESKTOP_NAMES"      , False );
	_NET_NUMBER_OF_DESKTOPS  = XInternAtom ( dpy , "_NET_NUMBER_OF_DESKTOPS" , False );
	_NET_DESKTOP_GEOMETRY    = XInternAtom ( dpy , "_NET_DESKTOP_GEOMETRY"   , False );
	UTF8_STRING              = XInternAtom ( dpy , "UTF8_STRING"              , False );

	// Detect the number of viewport rows via Xinerama or _NET_DESKTOP_GEOMETRY.
	if ( XineramaIsActive ( dpy ) )
	{
		int screen_count = 0; // number of Xinerama screens returned
		screens = XineramaQueryScreens ( dpy , & screen_count );
		if ( screens && screen_count > 0 )
		{
			num_viewports = screen_count;
			if ( debug ) printf ( "Xinerama is active, found %d screen(s).\n" , num_viewports );
		}
		else
		{
			num_viewports = 1;
			if ( debug ) printf ( "Xinerama active, but failed to query screens. Assuming 1.\n" );
		}
	}
	else
	{
		num_viewports = 1;
		screens       = NULL;
		if ( debug ) printf ( "Xinerama not active, assuming 1 screen.\n" );
	}

	// If Xinerama reported only 1 screen, check _NET_DESKTOP_GEOMETRY for virtual viewport rows.
	if ( num_viewports == 1 )
	{
		Atom          actual_type;   // actual property type returned
		int           actual_format; // bit width of the returned property data
		unsigned long nitems;        // number of items returned
		unsigned long bytes_after;   // remaining unread bytes (unused)
		unsigned char *gdata = NULL; // raw geometry property data

		XGetWindowProperty ( dpy , root , _NET_DESKTOP_GEOMETRY , 0 , 2 , False , XA_CARDINAL ,
		                     & actual_type , & actual_format , & nitems , & bytes_after , & gdata );

		if ( gdata )
		{
			if ( nitems >= 2 )
			{
				long virt_width  = ( ( long * ) gdata )[0]; // virtual desktop width in pixels
				long virt_height = ( ( long * ) gdata )[1]; // virtual desktop height in pixels

				int sw   = DisplayWidth  ( dpy , DefaultScreen ( dpy ) ); // physical screen width in pixels
				int sh   = DisplayHeight ( dpy , DefaultScreen ( dpy ) ); // physical screen height in pixels
				int h_vp = ( sw > 0 && virt_width  > sw ) ? ( int ) ( virt_width  / sw ) : 1; // number of viewport columns
				int v_vp = ( sh > 0 && virt_height > sh ) ? ( int ) ( virt_height / sh ) : 1; // number of viewport rows

				num_viewports = v_vp;

				// Clear screens[]: switch_viewport will use virtual row coordinates instead.
				if ( screens )
				{
					XFree ( screens );
					screens = NULL;
				}

				if ( debug ) printf ( "Virtual desktop geometry: %ldx%ld, screen: %dx%d -> %d viewport(s).\n" ,
				                      virt_width , virt_height , sw , sh , num_viewports );
				if ( debug ) printf ( "Virtual desktop grid: %d col x %d row (%ld/%d=%d, %ld/%d=%d).\n" ,
				                      h_vp , v_vp , virt_width , sw , h_vp , virt_height , sh , v_vp );
			}

			XFree ( gdata );
		}
	}

	// Save the original CapsLock keysym mapping so cleanup() can restore it.
	KeyCode caps_keycode        = XKeysymToKeycode ( dpy , XK_Caps_Lock ); // physical keycode of the CapsLock key
	if ( caps_keycode == 0 )
	{
		fprintf ( stderr , "apexswitcher: CapsLock key not found on this keyboard\n" );
		return 1;
	}
	int     keysyms_per_keycode = 0;                                         // number of keysyms per keycode slot
	memset ( original_mapping , 0 , sizeof ( original_mapping ) );
	KeySym *orig = XGetKeyboardMapping ( dpy , caps_keycode , 1 , & keysyms_per_keycode ); // original keysym mapping
	if ( orig && keysyms_per_keycode > 0 )
	{
		memcpy ( original_mapping , orig ,
		         sizeof ( KeySym ) * ( keysyms_per_keycode < 8 ? keysyms_per_keycode : 8 ) );
	}
	if ( orig ) XFree ( orig );

	// Remap CapsLock to Hyper_L and record the keycode globally.
	KeySym new_mapping[1] = { XK_Hyper_L }; // replacement keysym for the CapsLock key
	XChangeKeyboardMapping ( dpy , caps_keycode , 1 , new_mapping , 1 );
	XFlush ( dpy );
	hyper_keycode = caps_keycode; // same physical key, now sends Hyper_L

	// Load desktop names and the initial desktop/viewport state.
	fetch_workspace_names ();
	desktop_to_viewport_map = calloc ( num_desktops , sizeof ( int ) );
	if ( ! desktop_to_viewport_map ) { fprintf ( stderr , "apexswitcher: out of memory\n" ); exit ( 1 ); }
	refresh_current_state ();

	// Load the font used for all overlay rendering.
	font = XftFontOpenName ( dpy , screen_idx , "monospace-16" );
	if ( ! font )
	{
		fprintf ( stderr , "Failed to load font: monospace-16\n" );
		return 1;
	}

	// Precompute the window switcher row height from font metrics (font->ascent + descent + 12px gap).
	win_row_height = font->ascent + font->descent + 12;

	// Compute workspace overlay window dimensions from font metrics.
	int  spacing     = 10;                           // gap between desktop labels and viewport rows
	int  font_height = font->ascent + font->descent; // full font height in pixels
	int  row_padding = 10;                           // vertical padding within each row
	int  row_height  = font_height + row_padding;    // total pixel height of one row
	int  line_width  = 0;                            // accumulated pixel width of the label row
	char buf[128];                                   // desktop label scratch buffer

	// Measure the total label row width.
	for ( int i = 0 ; i < num_desktops ; ++ i )
	{
		if ( show_names )
		{
			snprintf ( buf , sizeof ( buf ) , "[%s]" , desktop_names[i] );
		}
		else
		{
			snprintf ( buf , sizeof ( buf ) , "[%02d]" , i + 1 );
		}
		XGlyphInfo extents; // glyph metrics for this label
		XftTextExtentsUtf8 ( dpy , font , ( XftChar8 * ) buf , strlen ( buf ) , & extents );
		line_width += extents.xOff + spacing;
	}
	line_width -= spacing;

	int win_width  = line_width + 60;                                                      // workspace overlay width in pixels
	int win_height = ( num_viewports * row_height ) + ( ( num_viewports - 1 ) * spacing ) + 20; // workspace overlay height in pixels

	// Center the workspace overlay on the primary screen.
	int win_x = ( DisplayWidth  ( dpy , screen_idx ) - win_width  ) / 2; // overlay X position
	int win_y = ( DisplayHeight ( dpy , screen_idx ) - win_height ) / 2; // overlay Y position
	if ( screens )
	{
		win_x = screens[0].x_org + ( screens[0].width  - win_width  ) / 2;
		win_y = screens[0].y_org + ( screens[0].height - win_height ) / 2;
	}

	// Create the workspace overlay window.
	XSetWindowAttributes attrs;                                   // window attribute struct
	attrs.override_redirect = True;                               // bypass the window manager
	attrs.background_pixel  = WhitePixel ( dpy , screen_idx );   // white background
	attrs.border_pixel      = BlackPixel ( dpy , screen_idx );   // black border

	win = XCreateWindow ( dpy , root , win_x , win_y , win_width , win_height , 1 ,
	                      CopyFromParent , InputOutput , DefaultVisual ( dpy , screen_idx ) ,
	                      CWOverrideRedirect | CWBackPixel | CWBorderPixel , & attrs );
	if ( ! win ) { fprintf ( stderr , "apexswitcher: failed to create workspace overlay window\n" ); return 1; }
	XStoreName ( dpy , win , "Workspace Overlay" );

	// Create the Xft drawing context and allocate the three text colors.
	draw = XftDrawCreate ( dpy , win , DefaultVisual ( dpy , screen_idx ) , DefaultColormap ( dpy , screen_idx ) );
	if ( ! draw ) { fprintf ( stderr , "apexswitcher: XftDrawCreate failed for workspace overlay\n" ); return 1; }

	XRenderColor xr_black     = { 0     , 0     , 0     , 65535 }; // black
	XRenderColor xr_highlight = { 35000 , 35000 , 60000 , 65535 }; // muted blue
	XRenderColor xr_grey      = { 16384 , 16384 , 16384 , 65535 }; // dark grey

	if ( ! XftColorAllocValue ( dpy , DefaultVisual ( dpy , screen_idx ) , DefaultColormap ( dpy , screen_idx ) , & xr_black     , & color           ) ||
	     ! XftColorAllocValue ( dpy , DefaultVisual ( dpy , screen_idx ) , DefaultColormap ( dpy , screen_idx ) , & xr_highlight , & highlight_color ) ||
	     ! XftColorAllocValue ( dpy , DefaultVisual ( dpy , screen_idx ) , DefaultColormap ( dpy , screen_idx ) , & xr_grey      , & grey_color      ) )
	{
		fprintf ( stderr , "apexswitcher: failed to allocate Xft text colors\n" );
		return 1;
	}

	// Allocate cell background colors and create one GC for each.
	XColor active_xc;   // color for the active viewport cell
	XColor inactive_xc; // color for inactive viewport cells on each desktop
	XColor dummy_xc;    // closest available color returned by the server (unused)
	if ( ! XAllocNamedColor ( dpy , DefaultColormap ( dpy , screen_idx ) , "yellow" , & active_xc   , & dummy_xc ) ||
	     ! XAllocNamedColor ( dpy , DefaultColormap ( dpy , screen_idx ) , "cyan"   , & inactive_xc , & dummy_xc ) )
	{
		fprintf ( stderr , "apexswitcher: failed to allocate cell background colors\n" );
		return 1;
	}
	XGCValues gcv;                          // GC attribute struct
	gcv.foreground = active_xc.pixel;       // active cell foreground pixel
	active_gc   = XCreateGC ( dpy , win , GCForeground , & gcv );
	gcv.foreground = inactive_xc.pixel;     // inactive cell foreground pixel
	inactive_gc = XCreateGC ( dpy , win , GCForeground , & gcv );
	if ( ! active_gc || ! inactive_gc ) { fprintf ( stderr , "apexswitcher: XCreateGC failed\n" ); return 1; }

	// Create the window switcher overlay (initially 1x1; resized each time it is shown).
	win2  = XCreateWindow ( dpy , root , 0 , 0 , 1 , 1 , 1 ,
	                        CopyFromParent , InputOutput , DefaultVisual ( dpy , screen_idx ) ,
	                        CWOverrideRedirect | CWBackPixel | CWBorderPixel , & attrs );
	if ( ! win2 ) { fprintf ( stderr , "apexswitcher: failed to create window switcher overlay\n" ); return 1; }
	XStoreName ( dpy , win2 , "Window Switcher" );
	draw2 = XftDrawCreate ( dpy , win2 , DefaultVisual ( dpy , screen_idx ) , DefaultColormap ( dpy , screen_idx ) );
	if ( ! draw2 ) { fprintf ( stderr , "apexswitcher: XftDrawCreate failed for window switcher\n" ); return 1; }
	XSelectInput ( dpy , win2 , ExposureMask );

	// Passively grab CapsLock (plain and Shift variants) for initial press detection.
	grab_key_with_mods ( hyper_keycode );

	unsigned int shift_mods[] = { ShiftMask , ShiftMask|Mod2Mask , ShiftMask|LockMask , ShiftMask|Mod2Mask|LockMask }; // Shift+CapsLock modifier variants

	for ( int i = 0 ; i < ( int ) ( sizeof ( shift_mods ) / sizeof ( shift_mods[0] ) ) ; ++ i )
	{
		XGrabKey ( dpy , hyper_keycode , shift_mods[i] , root , True , GrabModeAsync , GrabModeAsync );
	}

	// Select events on the root and overlay windows.
	XSelectInput ( dpy , root , KeyPressMask | KeyReleaseMask | PropertyChangeMask );
	XSelectInput ( dpy , win  , ExposureMask );

	// Main event loop: block on the X connection fd with a 1s timeout so that
	// signals (SIGINT/SIGTERM) can interrupt the wait within at most one second.
	int    xfd = XConnectionNumber ( dpy ); // file descriptor for the X connection
	XEvent ev;                              // current X event being processed

	while ( running )
	{
		if ( ! XPending ( dpy ) )
		{
			fd_set         fds; // file descriptor set for select()
			struct timeval tv;  // select() timeout

			FD_ZERO ( & fds );
			FD_SET ( xfd , & fds );
			tv.tv_sec  = 1;
			tv.tv_usec = 0;
			select ( xfd + 1 , & fds , NULL , NULL , & tv );
		}

		while ( XPending ( dpy ) && running )
		{
			XNextEvent ( dpy , & ev );

			// Redraw the appropriate overlay on expose events.
			if ( ev.type == Expose && showing )
			{
				draw_desktops ();
			}
			else
			{
				if ( ev.type == Expose && showing_windows )
				{
					draw_windows ();
				}
				else
				{
					if ( ev.type == KeyPress )
					{
						KeySym ks = XLookupKeysym ( & ev.xkey , 0 ); // keysym for the pressed key

						// CapsLock pressed while no overlay is open: show the appropriate one.
						if ( ev.xkey.keycode == hyper_keycode && ! showing && ! showing_windows )
						{
							if ( ev.xkey.state & ShiftMask )
							{
								// Shift+CapsLock: show the window switcher overlay.
								refresh_current_state ();
								fetch_windows ();

								int  row_h    = win_row_height; // pixel height of one list row
								int  w2_height = 0;             // switcher overlay height (set below)
								int  w2_width  = 0;             // switcher overlay width (set below)

								if ( num_windows == 0 )
								{
									const char *no_win_msg = "(no windows in current workspace)";
									XGlyphInfo  no_win_ext;
									XftTextExtentsUtf8 ( dpy , font , ( XftChar8 * ) no_win_msg ,
									                     strlen ( no_win_msg ) , & no_win_ext );
									w2_width  = no_win_ext.xOff + 30;
									w2_height = row_h + 30;
								}
								else
								{
									char tbuf[512]; // label + title scratch buffer
									// Measure the widest entry to determine the overlay width.
									for ( int i = 0 ; i < num_windows ; ++ i )
									{
										fmt_window_label ( tbuf , sizeof ( tbuf ) , i , window_names[i] );
										XGlyphInfo ext;
										XftTextExtentsUtf8 ( dpy , font , ( XftChar8 * ) tbuf , strlen ( tbuf ) , & ext );
										if ( ext.xOff > w2_width )
										{
											w2_width = ext.xOff;
										}
									}
									w2_width  += 30;
									w2_height  = num_windows * row_h + 30;
								}

								// Center, resize, and show the overlay on the active screen.
								int scr2   = screen_for_active_window ();                                                  // index of the target screen
								int scr2_x = screens ? screens[scr2].x_org  : 0;                                          // target screen X origin
								int scr2_y = screens ? screens[scr2].y_org  : 0;                                          // target screen Y origin
								int scr2_w = screens ? screens[scr2].width  : DisplayWidth  ( dpy , screen_idx );         // target screen width
								int scr2_h = screens ? screens[scr2].height : DisplayHeight ( dpy , screen_idx );         // target screen height
								int w2_x   = scr2_x + ( scr2_w - w2_width  ) / 2;                                        // switcher overlay X position
								int w2_y   = scr2_y + ( scr2_h - w2_height ) / 2;                                        // switcher overlay Y position
								XMoveResizeWindow ( dpy , win2 , w2_x , w2_y , w2_width , w2_height );
								XMapRaised ( dpy , win2 );
								draw_windows ();

								if ( try_grab_keyboard () == GrabSuccess )
								{
									showing_windows = 1;
								}
								else
								{
									XUnmapWindow ( dpy , win2 );
									XFlush ( dpy );
								}
							}
							else
							{
								// CapsLock alone: show the workspace/viewport overlay on the active screen.
								refresh_current_state ();

								// Reposition the overlay to the screen containing the active window.
								int    scr   = screen_for_active_window ();                    // index of the target screen
								int    scr_x = screens ? screens[scr].x_org    : 0;            // target screen X origin
								int    scr_y = screens ? screens[scr].y_org    : 0;            // target screen Y origin
								int    scr_w = screens ? screens[scr].width    : DisplayWidth  ( dpy , screen_idx ); // target screen width
								int    scr_h = screens ? screens[scr].height   : DisplayHeight ( dpy , screen_idx ); // target screen height
								XWindowAttributes wwa;                                                   // current overlay dimensions
								if ( ! XGetWindowAttributes ( dpy , win , & wwa ) ) { wwa.width = 0; wwa.height = 0; }
								int    ox = scr_x + ( scr_w - wwa.width  ) / 2;               // centred overlay X
								int    oy = scr_y + ( scr_h - wwa.height ) / 2;               // centred overlay Y
								XMoveWindow ( dpy , win , ox , oy );

								XMapRaised ( dpy , win );
								draw_desktops ();

								if ( try_grab_keyboard () == GrabSuccess )
								{
									showing = 1;
								}
								else
								{
									XUnmapWindow ( dpy , win );
									XFlush ( dpy );
								}
							}
						}
						else
						{
							// Handle navigation keys while the workspace overlay is open.
							if ( showing )
							{
								int new_viewport = -1; // requested viewport row (-1 = no change)
								int new_desktop  = -1; // requested desktop index (-1 = no change)

								switch ( ks )
								{

									case XK_Left:

										if ( current_desktop > 0 )
										{
											new_desktop = current_desktop - 1;
										}

									break;

									case XK_Right:

										if ( current_desktop < num_desktops - 1 )
										{
											new_desktop = current_desktop + 1;
										}

									break;

									case XK_Up:

										if ( debug ) printf ( "[keypress] Up: current_viewport=%d num_viewports=%d\n" ,
									                      current_viewport , num_viewports );
										if ( current_viewport > 0 )
										{
											new_viewport = current_viewport - 1;
											if ( debug ) printf ( "[keypress] -> new_viewport=%d\n" , new_viewport );
										}

									break;

									case XK_Down:

										if ( debug ) printf ( "[keypress] Down: current_viewport=%d num_viewports=%d\n" ,
									                      current_viewport , num_viewports );
										if ( current_viewport < num_viewports - 1 )
										{
											new_viewport = current_viewport + 1;
											if ( debug ) printf ( "[keypress] -> new_viewport=%d\n" , new_viewport );
										}

									break;

									case XK_1: case XK_2: case XK_3: case XK_4: case XK_5:
									case XK_6: case XK_7: case XK_8: case XK_9: case XK_0:
									{

										int target = ( ks == XK_0 ) ? 9 : ( int ) ( ks - XK_1 ); // zero-based desktop index for this key
										if ( target < num_desktops )
										{
											new_desktop = target;
										}

									}
									break;

								}

								// Apply the requested desktop change.
								if ( new_desktop != -1 )
								{
									if ( debug ) printf ( "[desktop] switching to %d\n" , new_desktop );
									switch_desktop ( new_desktop );
									current_desktop = new_desktop;
									draw_desktops ();
								}

								// Apply the requested viewport change.
								if ( new_viewport != -1 )
								{
									if ( debug ) printf ( "[viewport] switching to %d\n" , new_viewport );
									switch_viewport ( new_viewport );
									current_viewport = new_viewport;
									draw_desktops ();
								}
							}
							else
							{
								// Handle navigation keys while the window switcher is open.
								if ( showing_windows )
								{
									switch ( ks )
									{

										case XK_Up:

											if ( current_window_idx > 0 )
											{
												current_window_idx --;
												activate_window_by_id ( window_ids[current_window_idx] );
												draw_windows ();
											}

										break;

										case XK_Down:

											if ( current_window_idx < num_windows - 1 )
											{
												current_window_idx ++;
												activate_window_by_id ( window_ids[current_window_idx] );
												draw_windows ();
											}

										break;

										case XK_1: case XK_2: case XK_3: case XK_4: case XK_5:
										case XK_6: case XK_7: case XK_8: case XK_9: case XK_0:
										{

											int target = ( ks == XK_0 ) ? 9 : ( int ) ( ks - XK_1 ); // zero-based window index for this number key
											if ( target < num_windows )
											{
												current_window_idx = target;
												activate_window_by_id ( window_ids[current_window_idx] );
												draw_windows ();
											}

										}
										break;

										case XK_F1:  case XK_F2:  case XK_F3:  case XK_F4:
										case XK_F5:  case XK_F6:  case XK_F7:  case XK_F8:
										case XK_F9:  case XK_F10: case XK_F11: case XK_F12:
										{

											int target = 10 + ( int ) ( ks - XK_F1 ); // F1 maps to index 10, F12 to index 21
											if ( target < num_windows )
											{
												current_window_idx = target;
												activate_window_by_id ( window_ids[current_window_idx] );
												draw_windows ();
											}

										}
										break;

									}
								}
							}
						}
					}
					else
					{
						// CapsLock released while the workspace overlay is open: hide it.
						if ( ev.type == KeyRelease && ev.xkey.keycode == hyper_keycode && showing )
						{
							XUngrabKeyboard ( dpy , CurrentTime );
							XUnmapWindow ( dpy , win );
							XFlush ( dpy );
							showing = 0;
						}
						else
						{
							// CapsLock released while the window switcher is open: hide it.
							if ( ev.type == KeyRelease && ev.xkey.keycode == hyper_keycode && showing_windows )
							{
								XUngrabKeyboard ( dpy , CurrentTime );
								XUnmapWindow ( dpy , win2 );
								XFlush ( dpy );
								showing_windows = 0;
							}
							else
							{
								// Property change while the workspace overlay is open: refresh and redraw.
								if ( ev.type == PropertyNotify && showing )
								{
									if ( ev.xproperty.atom == _NET_CURRENT_DESKTOP ||
									     ev.xproperty.atom == _NET_DESKTOP_VIEWPORT )
									{
										refresh_current_state ();
										draw_desktops ();
									}
								}
							}
						}
					}
				}
			}
		}
	}

	return 0;
}
