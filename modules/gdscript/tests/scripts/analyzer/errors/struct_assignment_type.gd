struct First:
	var value: int

struct Second:
	var value: int

func test():
	var first: First = First()
	var second: Second = Second()
	first = second
