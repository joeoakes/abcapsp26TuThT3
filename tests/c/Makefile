CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Ivendor/unity/src -I../../maze

UNITY_SRC = vendor/unity/src/unity.c
MAZE_SRC = ../../maze/maze_lib.c
TEST_SRC = test_maze_lib.c
TARGET = test_maze_lib

all: $(TARGET)

$(TARGET): $(TEST_SRC) $(MAZE_SRC) $(UNITY_SRC)
	$(CC) $(CFLAGS) $(TEST_SRC) $(MAZE_SRC) $(UNITY_SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
