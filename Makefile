CC = gcc

CFLAGS = -Wall

OBJECTS = COR.o

NAME = COR

$(NAME): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $(NAME)

COR.o: COR.c COR.h

clean:
	rm -f $(NAME) *.o