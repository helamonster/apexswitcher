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
char              **desktop_names      = NULL;    // array of desktop name strings
int                 num_desktops       = 0;       // total number of desktops
int                 current_desktop    = 0;       // index of the currently active desktop
int                 current_viewport   = 0;       // current viewport row index
int                 num_viewports      = 1;       // number of viewport rows
long                current_vx         = 0;       // current viewport X offset (preserves column on row switch)
int                 showing            = 0;       // 1 while the workspace overlay is visible
int                 show_names         = 0;       // 1 if -s flag: show desktop names instead of numbers

Atom _NET_CURRENT_DESKTOP;                       // atom: read/set the active desktop
Atom _NET_DESKTOP_VIEWPORT;                      // atom: read/set the viewport scroll position
Atom _NET_CLIENT_LIST;                           // atom: list of managed client windows
Atom _NET_WM_DESKTOP;                            // atom: which desktop a window belongs to
Atom _NET_WM_NAME;                               // atom: UTF-8 window title
Atom _NET_ACTIVE_WINDOW;                         // atom: send a window-activation request

Window              win2               = 0;       // window switcher overlay window
XftDraw            *draw2              = NULL;    // Xft drawing context for the window switcher
Window             *window_ids         = NULL;    // Window IDs on the current desktop/viewport
char              **window_names       = NULL;    // title strings matching window_ids[]
int                 num_windows        = 0;       // number of entries in window_ids/window_names
int                 current_window_idx = 0;       // index of the highlighted window in the switcher
int                 showing_windows    = 0;       // 1 while the window switcher overlay is visible

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
		if ( draw )  XftDrawDestroy ( draw );
		if ( draw2 ) XftDrawDestroy ( draw2 );
		if ( font )  XftFontClose ( dpy , font );

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

	running = 0;
}


////////////////////////////////////////////////////////////////////////////////


// Passively grabs a key with all common modifier combinations (NumLock, CapsLock, both).
void grab_key_with_mods ( Display *dpy , Window root , int keycode )
{

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
void fetch_workspace_names ( Display *dpy , Window root )
{

	Atom net_desktop_names = XInternAtom ( dpy , "_NET_DESKTOP_NAMES" , False ); // atom for the desktop name list
	Atom utf8              = XInternAtom ( dpy , "UTF8_STRING"         , False ); // atom for the UTF-8 string type

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
	XGetWindowProperty ( dpy , root , net_desktop_names , 0 , 4096 , False , utf8 ,
	                     & actual_type , & actual_format , & nitems , & bytes_after , & data );

	if ( data && nitems > 0 )
	{
		// Parse the null-separated UTF-8 string list into desktop_names[].
		char *ptr = ( char * ) data; // cursor through the null-separated property data

		while ( ( unsigned char * ) ptr < data + nitems )
		{
			desktop_names = realloc ( desktop_names , ( num_desktops + 1 ) * sizeof ( char * ) );
			desktop_names[num_desktops ++] = strdup ( ptr );
			ptr += strlen ( ptr ) + 1;
		}
		XFree ( data );
	}
	else
	{
		// Fall back to _NET_NUMBER_OF_DESKTOPS and generate "Desktop N" names.
		Atom          net_number_of_desktops = XInternAtom ( dpy , "_NET_NUMBER_OF_DESKTOPS" , False ); // desktop count atom
		unsigned char *ndata                 = NULL;                                                      // raw desktop-count property data

		XGetWindowProperty ( dpy , root , net_number_of_desktops , 0 , 1 , False , XA_CARDINAL ,
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

		for ( int i = 0 ; i < num_desktops ; ++ i )
		{
			char buf[32]; // scratch buffer for the generated name
			snprintf ( buf , sizeof ( buf ) , "Desktop %d" , i + 1 );
			desktop_names[i] = strdup ( buf );
		}
	}
}


////////////////////////////////////////////////////////////////////////////////


// Reads _NET_CURRENT_DESKTOP and _NET_DESKTOP_VIEWPORT into the global state variables.
void refresh_current_state ( Display *dpy , Window root )
{

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

	// Read the (x, y) viewport offset for the current desktop.
	unsigned char *vpdata = NULL; // raw _NET_DESKTOP_VIEWPORT property data
	XGetWindowProperty ( dpy , root , _NET_DESKTOP_VIEWPORT , current_desktop * 2 , 2 , False , XA_CARDINAL ,
	                     & actual_type , & actual_format , & nitems , & bytes_after , & vpdata );

	if ( vpdata && nitems >= 2 )
	{
		long vp_x = ( ( long * ) vpdata )[0]; // horizontal scroll offset in pixels
		long vp_y = ( ( long * ) vpdata )[1]; // vertical scroll offset in pixels
		XFree ( vpdata );
		current_vx = vp_x;

		int found_viewport = 0; // set to 1 once the matching viewport row is identified

		// Xinerama mode: match the offset against physical screen origins.
		if ( screens )
		{
			for ( int i = 0 ; i < num_viewports ; ++ i )
			{
				if ( vp_x == screens[i].x_org && vp_y == screens[i].y_org )
				{
					current_viewport = i;
					found_viewport   = 1;
					break;
				}
			}
		}

		// Virtual viewport mode: derive row index by dividing Y offset by screen height.
		if ( ! found_viewport )
		{
			long screen_height = DisplayHeight ( dpy , DefaultScreen ( dpy ) ); // physical screen height in pixels
			if ( screen_height > 0 )
			{
				current_viewport = vp_y / screen_height;
			}
		}
	}

	// Record the current viewport for this desktop.
	if ( desktop_to_viewport_map )
	{
		desktop_to_viewport_map[current_desktop] = current_viewport;
	}
}


////////////////////////////////////////////////////////////////////////////////


// Sends a _NET_CURRENT_DESKTOP ClientMessage to switch to the given workspace.
void switch_desktop ( Display *dpy , Window root , int new_desktop )
{

	XEvent ev = {0}; // event to send to the window manager

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
void switch_viewport ( Display *dpy , Window root , int viewport_index )
{

	if ( viewport_index < 0 || viewport_index >= num_viewports )
	{
		return;
	}

	// Compute target coordinates: preserve the horizontal column, change the row.
	long vp_x = screens ? screens[viewport_index].x_org : current_vx;                                                              // target X offset in pixels
	long vp_y = screens ? screens[viewport_index].y_org : ( long ) viewport_index * DisplayHeight ( dpy , DefaultScreen ( dpy ) ); // target Y offset in pixels

	printf ( "[switch_viewport] index=%d -> sending x=%ld y=%ld\n" , viewport_index , vp_x , vp_y );

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


// Returns the pixel height of one row in the window switcher list.
int win_row_height ( void )
{

	// font->ascent + font->descent + 8 px padding + 4 px row gap.
	return font->ascent + font->descent + 12;
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

	Atom           utf8          = XInternAtom ( dpy , "UTF8_STRING" , False ); // UTF-8 string type atom
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
			XGetWindowAttributes ( dpy , w , & wa );
			int win_bottom = wy + ( int ) wa.height; // Y coordinate of the window's bottom edge
			if ( win_bottom <= 0 || wy >= sh )
			{
				continue;
			}
		}

		// Get the window title, preferring UTF-8 _NET_WM_NAME over the legacy XFetchName.
		char          *title     = NULL; // heap-allocated window title string
		unsigned char *name_data = NULL; // raw _NET_WM_NAME property data
		XGetWindowProperty ( dpy , w , _NET_WM_NAME , 0 , 256 , False , utf8 ,
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

		// Append this window to the output lists.
		window_ids   = realloc ( window_ids   , ( num_windows + 1 ) * sizeof ( Window ) );
		window_names = realloc ( window_names , ( num_windows + 1 ) * sizeof ( char * ) );
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

	int  row_height = win_row_height (); // pixel height of one list row
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
	XGetWindowAttributes ( dpy , win , & wa );

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

			// Highlight the label for the active desktop on the active viewport row.
			int       active = ( i == current_desktop && v == current_viewport ); // 1 if this cell is current
			XftColor *clr    = active ? & highlight_color : & color;               // color based on active state
			XftDrawStringUtf8 ( draw , clr , font , x , y_baseline ,
			                    ( XftChar8 * ) buf , strlen ( buf ) );
			x += extents.xOff + spacing;
		}
	}
}


////////////////////////////////////////////////////////////////////////////////


// Attempts XGrabKeyboard; prints the specific reason to stderr on failure.
// Returns GrabSuccess on success, or the X grab error code on failure.
static int try_grab_keyboard ( Display *dpy )
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
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////


// Initializes the display, remaps CapsLock, creates overlay windows, and runs the event loop.
int main ( int argc , char *argv[] )
{

	// Parse command-line flags.
	if ( argc > 1 && strcmp ( argv[1] , "-s" ) == 0 )
	{
		show_names = 1;
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
	_NET_CURRENT_DESKTOP  = XInternAtom ( dpy , "_NET_CURRENT_DESKTOP"  , False );
	_NET_DESKTOP_VIEWPORT = XInternAtom ( dpy , "_NET_DESKTOP_VIEWPORT" , False );
	_NET_CLIENT_LIST      = XInternAtom ( dpy , "_NET_CLIENT_LIST"      , False );
	_NET_WM_DESKTOP       = XInternAtom ( dpy , "_NET_WM_DESKTOP"       , False );
	_NET_WM_NAME          = XInternAtom ( dpy , "_NET_WM_NAME"          , False );
	_NET_ACTIVE_WINDOW    = XInternAtom ( dpy , "_NET_ACTIVE_WINDOW"    , False );

	// Detect the number of viewport rows via Xinerama or _NET_DESKTOP_GEOMETRY.
	if ( XineramaIsActive ( dpy ) )
	{
		int screen_count = 0; // number of Xinerama screens returned
		screens = XineramaQueryScreens ( dpy , & screen_count );
		if ( screens && screen_count > 0 )
		{
			num_viewports = screen_count;
			printf ( "Xinerama is active, found %d screen(s).\n" , num_viewports );
		}
		else
		{
			num_viewports = 1;
			printf ( "Xinerama active, but failed to query screens. Assuming 1.\n" );
		}
	}
	else
	{
		num_viewports = 1;
		screens       = NULL;
		printf ( "Xinerama not active, assuming 1 screen.\n" );
	}

	// If Xinerama reported only 1 screen, check _NET_DESKTOP_GEOMETRY for virtual viewport rows.
	if ( num_viewports == 1 )
	{
		Atom          net_desktop_geometry = XInternAtom ( dpy , "_NET_DESKTOP_GEOMETRY" , False ); // atom for the virtual desktop size
		Atom          actual_type;   // actual property type returned
		int           actual_format; // bit width of the returned property data
		unsigned long nitems;        // number of items returned
		unsigned long bytes_after;   // remaining unread bytes (unused)
		unsigned char *gdata = NULL; // raw geometry property data

		XGetWindowProperty ( dpy , root , net_desktop_geometry , 0 , 2 , False , XA_CARDINAL ,
		                     & actual_type , & actual_format , & nitems , & bytes_after , & gdata );

		if ( gdata && nitems >= 2 )
		{
			long virt_width  = ( ( long * ) gdata )[0]; // virtual desktop width in pixels
			long virt_height = ( ( long * ) gdata )[1]; // virtual desktop height in pixels
			XFree ( gdata );

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

			printf ( "Virtual desktop geometry: %ldx%ld, screen: %dx%d -> %d viewport(s).\n" ,
			         virt_width , virt_height , sw , sh , num_viewports );
			printf ( "You have a %d-column x %d-row viewport grid (%ld/%d=%d, %ld/%d=%d).\n" ,
			         h_vp , v_vp , virt_width , sw , h_vp , virt_height , sh , v_vp );
		}
	}

	// Save the original CapsLock keysym mapping so cleanup() can restore it.
	KeyCode caps_keycode        = XKeysymToKeycode ( dpy , XK_Caps_Lock ); // physical keycode of the CapsLock key
	int     keysyms_per_keycode;                                             // number of keysyms per keycode slot
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
	fetch_workspace_names ( dpy , root );
	desktop_to_viewport_map = calloc ( num_desktops , sizeof ( int ) );
	refresh_current_state ( dpy , root );

	// Load the font used for all overlay rendering.
	font = XftFontOpenName ( dpy , screen_idx , "monospace-16" );
	if ( ! font )
	{
		fprintf ( stderr , "Failed to load font: monospace-16\n" );
		return 1;
	}

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
	XStoreName ( dpy , win , "Workspace Overlay" );

	// Create the Xft drawing context and allocate the three colors.
	draw = XftDrawCreate ( dpy , win , DefaultVisual ( dpy , screen_idx ) , DefaultColormap ( dpy , screen_idx ) );

	XRenderColor xr_black     = { 0     , 0     , 0     , 65535 }; // black
	XRenderColor xr_highlight = { 35000 , 35000 , 60000 , 65535 }; // muted blue
	XRenderColor xr_grey      = { 16384 , 16384 , 16384 , 65535 }; // dark grey

	XftColorAllocValue ( dpy , DefaultVisual ( dpy , screen_idx ) , DefaultColormap ( dpy , screen_idx ) , & xr_black     , & color           );
	XftColorAllocValue ( dpy , DefaultVisual ( dpy , screen_idx ) , DefaultColormap ( dpy , screen_idx ) , & xr_highlight , & highlight_color );
	XftColorAllocValue ( dpy , DefaultVisual ( dpy , screen_idx ) , DefaultColormap ( dpy , screen_idx ) , & xr_grey      , & grey_color      );

	// Create the window switcher overlay (initially 1x1; resized each time it is shown).
	win2  = XCreateWindow ( dpy , root , 0 , 0 , 1 , 1 , 1 ,
	                        CopyFromParent , InputOutput , DefaultVisual ( dpy , screen_idx ) ,
	                        CWOverrideRedirect | CWBackPixel | CWBorderPixel , & attrs );
	XStoreName ( dpy , win2 , "Window Switcher" );
	draw2 = XftDrawCreate ( dpy , win2 , DefaultVisual ( dpy , screen_idx ) , DefaultColormap ( dpy , screen_idx ) );
	XSelectInput ( dpy , win2 , ExposureMask );

	// Passively grab CapsLock (plain and Shift variants) for initial press detection.
	grab_key_with_mods ( dpy , root , hyper_keycode );

	unsigned int shift_mods[] = { ShiftMask , ShiftMask|Mod2Mask , ShiftMask|LockMask , ShiftMask|Mod2Mask|LockMask }; // Shift+CapsLock modifier variants

	for ( int i = 0 ; i < 4 ; ++ i )
	{
		XGrabKey ( dpy , hyper_keycode , shift_mods[i] , root , True , GrabModeAsync , GrabModeAsync );
	}

	// Select events on the root and overlay windows.
	XSelectInput ( dpy , root , KeyPressMask | KeyReleaseMask | PropertyChangeMask );
	XSelectInput ( dpy , win  , ExposureMask );

	// Main event loop.
	XEvent ev; // current X event being processed
	while ( running )
	{
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
								refresh_current_state ( dpy , root );
								fetch_windows ();

								int  row_h     = win_row_height ();        // pixel height of one list row
								int  w2_height = num_windows * row_h + 30; // switcher overlay height
								int  w2_width  = 0;                         // switcher overlay width (computed below)
								char tbuf[512];                              // label + title scratch buffer

								// Measure the widest entry to determine the overlay width.
								for ( int i = 0 ; i < num_windows ; ++ i )
								{
									fmt_window_label ( tbuf , sizeof ( tbuf ) , i , window_names[i] );
									XGlyphInfo ext; // glyph metrics for this entry
									XftTextExtentsUtf8 ( dpy , font , ( XftChar8 * ) tbuf , strlen ( tbuf ) , & ext );
									if ( ext.xOff > w2_width )
									{
										w2_width = ext.xOff;
									}
								}
								w2_width += 30;

								// When the list is empty, size to fit the placeholder message instead.
								if ( num_windows == 0 )
								{
									const char *no_win_msg = "(no windows in current workspace)"; // placeholder text
									XGlyphInfo  no_win_ext;                                        // glyph metrics for the placeholder
									XftTextExtentsUtf8 ( dpy , font , ( XftChar8 * ) no_win_msg ,
									                     strlen ( no_win_msg ) , & no_win_ext );
									w2_width  = no_win_ext.xOff + 30;
									w2_height = row_h + 30;
								}

								// Center, resize, and show the overlay.
								int w2_x = ( DisplayWidth  ( dpy , screen_idx ) - w2_width  ) / 2; // switcher overlay X position
								int w2_y = ( DisplayHeight ( dpy , screen_idx ) - w2_height ) / 2; // switcher overlay Y position
								XMoveResizeWindow ( dpy , win2 , w2_x , w2_y , w2_width , w2_height );
								XMapRaised ( dpy , win2 );
								draw_windows ();

								if ( try_grab_keyboard ( dpy ) == GrabSuccess )
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
								// CapsLock alone: show the workspace/viewport overlay.
								refresh_current_state ( dpy , root );
								XMapRaised ( dpy , win );
								draw_desktops ();

								if ( try_grab_keyboard ( dpy ) == GrabSuccess )
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

										printf ( "\n[keypress] Up key pressed. Current viewport: %d ; Num viewports: %d\n" ,
										         current_viewport , num_viewports );
										if ( current_viewport > 0 )
										{
											new_viewport = current_viewport - 1;
											printf ( "\nWill change viewport to %d...\n" , new_viewport );
										}

									break;

									case XK_Down:

										printf ( "\n[keypress] Down key pressed. Current viewport: %d ; Num viewports: %d\n" ,
										         current_viewport , num_viewports );
										if ( current_viewport < num_viewports - 1 )
										{
											new_viewport = current_viewport + 1;
											printf ( "\nWill change viewport to %d...\n" , new_viewport );
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
									printf ( "\nChanging workspace to %d...\n" , new_desktop );
									switch_desktop ( dpy , root , new_desktop );
									current_desktop = new_desktop;
									draw_desktops ();
								}

								// Apply the requested viewport change.
								if ( new_viewport != -1 )
								{
									printf ( "\nChanging viewport to %d...\n" , new_viewport );
									switch_viewport ( dpy , root , new_viewport );
									current_viewport = new_viewport;
									draw_desktops ();
								}
							}
							else
							{
								// Handle navigation keys while the window switcher is open.
								if ( showing_windows )
								{
									int target = -1; // target window index (-1 = no change)

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

											target = ( ks == XK_0 ) ? 9 : ( int ) ( ks - XK_1 ); // zero-based window index for this number key
											if ( target < num_windows )
											{
												current_window_idx = target;
												activate_window_by_id ( window_ids[current_window_idx] );
												draw_windows ();
											}

										break;

										case XK_F1:  case XK_F2:  case XK_F3:  case XK_F4:
										case XK_F5:  case XK_F6:  case XK_F7:  case XK_F8:
										case XK_F9:  case XK_F10: case XK_F11: case XK_F12:

											target = 10 + ( int ) ( ks - XK_F1 ); // F1 maps to index 10, F12 to index 21
											if ( target < num_windows )
											{
												current_window_idx = target;
												activate_window_by_id ( window_ids[current_window_idx] );
												draw_windows ();
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
										refresh_current_state ( dpy , root );
										draw_desktops ();
									}
								}
							}
						}
					}
				}
			}
		}
		usleep ( 10000 );
	}

	return 0;
}
