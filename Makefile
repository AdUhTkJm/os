# Required by the competition.
# Use test.py for development.
all:
# We must run it a first time to set up symtbl.inc.
	./test.py
	./test.py --no-debug-memory --no-instrument --no-debug --rebuild -r
	