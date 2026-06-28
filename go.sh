do_prep()
{
	sudo apt install libx11-dev libxft-dev build-essential libfreetype-dev
	sudo apt install libxft-dev libxinerama-dev

}

DEBUG=1

do_build()
{

	if [ $DEBUG -eq 1 ] ; then
		DEBUG_ARGS=" -g -O0 "
	else
		DEBUG_ARGS=""
	fi

#	gcc -o superswitcher_test superswitcher_test.c -lX11 -lXft
#	gcc -o superswitcher_test superswitcher_test.c -lX11 -lXft -I/usr/include/freetype2
	gcc -o superswitcher_test superswitcher_test.c $(pkg-config --cflags --libs x11 xft)

	gcc $DEBUG_ARGS -o caps_event caps_event.c  $(pkg-config --cflags --libs x11 xft xinerama)

}


