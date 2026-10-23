# The Terminal folder on the microSD (Developer options > Terminal binaries):
# what is in its bin folder runs by name from an ADB shell as well as from the
# Terminal app. /etc/profile sets PATH before it reads this, so it has to be
# added here.
if [ -d /mnt/sd_0/Terminal/bin ]; then
	export PATH="/mnt/sd_0/Terminal/bin:$PATH"
	export LD_LIBRARY_PATH="/mnt/sd_0/Terminal/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
