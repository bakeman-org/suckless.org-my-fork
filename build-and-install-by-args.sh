# this is linux build script, not for bsd
set -xe
if [ -f stest.c ]; then
    gcc -o stest stest.c -Wall -Wextra
fi
gcc -o dmenu dmenux2.c `pkg-config --cflags --libs freetype2` -lX11  -lXft -lfontconfig -DXINERAMA -lXinerama -Wall -Wextra -pedantic -D_XOPEN_SOURCE=700  -std=c11 -DDMENU_CENTER_PATCH -DGRUBER_DARKER_PATCH "$@"


sudo cp dmenu /usr/local/bin/dmenu
for f in dmenu_run dmenu_path stest; do
    if [ -f "$f" ]; then
        sudo cp "$f" /usr/local/bin/"$f"
    fi
done

############################### Below is document ###########################################################
### currently example build args
# ./build-and-install-by-args.sh -DDMENU_CENTER_PATCH -DUSER_PROMPT_PATCH -DPLUS_DEFAULT_PROMPT
