class ValueA:
	func value():
		return "A"

class ValueB:
	func value():
		return "B"

class ValueC:
	func value():
		return "C"

class ValueD:
	func value():
		return "D"

class ValueE:
	func value():
		return "E"

func dynamic_add(left, right):
	return left + right

func call_value(instance):
	return instance.value()

func call_reload_value(instance):
	return instance.value()

func test():
	Utils.check(dynamic_add(1, 2) == 3)
	Utils.check(dynamic_add(1.5, 2.5) == 4.0)
	Utils.check(dynamic_add("a", "b") == "ab")
	Utils.check(dynamic_add(Vector2(1, 2), Vector2(3, 4)) == Vector2(4, 6))
	Utils.check(dynamic_add([1], [2]) == [1, 2])
	# Revisit cached and megamorphic signatures.
	Utils.check(dynamic_add(4, 5) == 9)
	Utils.check(dynamic_add([3], [4]) == [3, 4])
	print("operator feedback: ok")

	var instances = [ValueA.new(), ValueB.new(), ValueC.new(), ValueD.new(), ValueE.new()]
	var results = []
	for instance in instances:
		results.append(call_value(instance))
	# Revisit cached and megamorphic receiver scripts.
	results.append(call_value(instances[0]))
	results.append(call_value(instances[4]))
	Utils.check(results == ["A", "B", "C", "D", "E", "A", "E"])
	print("call feedback: ok")

	var dynamic_script := GDScript.new()
	dynamic_script.source_code = "extends RefCounted\nfunc value():\n\treturn 10\n"
	Utils.check(dynamic_script.reload() == OK)
	var dynamic_instance := RefCounted.new()
	dynamic_instance.set_script(dynamic_script)
	Utils.check(call_reload_value(dynamic_instance) == 10)
	dynamic_script.source_code = "extends RefCounted\nfunc value():\n\treturn 20\n"
	Utils.check(dynamic_script.reload(true) == OK)
	Utils.check(call_reload_value(dynamic_instance) == 20)
	print("call feedback invalidation: ok")
