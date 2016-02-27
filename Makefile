hello_world: hello_world.cc
	bash ./decompress_libseastar.sh
	g++ `pkg-config --cflags --libs ./seastar.pc` hello_world.cc -o hello_world

.PHONY: clean

clean:
	rm -f hello_world libseastar.a
