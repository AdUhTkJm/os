# Required by the competition.
# Use test.py for development.
all:
	./test.py --release --remote-test --rebuild
	mv build/kernel kernel-rv
	