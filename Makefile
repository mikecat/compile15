CC=gcc
CXX=g++
LEX=flex
YACC=bison
CFLAGS=-O2 -Wall -Wextra -pedantic -std=c99
CXXFLAGS=-O2 -Wall -Wextra -pedantic -std=c++11
LDFLAGS=

TARGET=compile15
OBJS=compile15_lex.o compile15_parse.o compile15_main.o util.o

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

compile15_lex.o: compile15_lex.c
	$(CC) $(CFLAGS) -Wno-unused-function -c -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $^

compile15_lex.c: compile15.l
	$(LEX) -o$@ $^

compile15_parse.c: compile15.y
	$(YACC) -d -o$@ $^

.PHONY: clean
clean:
	rm -f $(TARGET) $(OBJS) compile15_lex.c compile15_parse.c