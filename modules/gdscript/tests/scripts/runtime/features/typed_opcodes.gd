func test():
	var left_int: int = 12
	var right_int: int = 3
	Utils.check(left_int + right_int == 15)
	Utils.check(left_int - right_int == 9)
	Utils.check(left_int * right_int == 36)
	Utils.check(right_int ** 3 == 27)
	Utils.check(-right_int == -3)
	Utils.check(+right_int == 3)
	Utils.check(right_int << 2 == 12)
	Utils.check(left_int >> 2 == 3)
	Utils.check((left_int | 1) == 13)
	Utils.check((left_int & 10) == 8)
	Utils.check((left_int ^ 10) == 6)
	Utils.check(~right_int == -4)
	Utils.check(left_int == 12)
	Utils.check(left_int != right_int)
	Utils.check(right_int < left_int)
	Utils.check(right_int <= 3)
	Utils.check(left_int > right_int)
	Utils.check(left_int >= 12)

	var left_float: float = 12.0
	var right_float: float = 3.0
	Utils.check(left_float + right_float == 15.0)
	Utils.check(left_float - right_float == 9.0)
	Utils.check(left_float * right_float == 36.0)
	Utils.check(left_float / right_float == 4.0)
	Utils.check(right_float ** 2.0 == 9.0)
	Utils.check(-right_float == -3.0)
	Utils.check(+right_float == 3.0)
	Utils.check(left_float == 12.0)
	Utils.check(left_float != right_float)
	Utils.check(right_float < left_float)
	Utils.check(right_float <= 3.0)
	Utils.check(left_float > right_float)
	Utils.check(left_float >= 12.0)

	var bool_condition: bool = true
	var bool_branch: int = 0
	if bool_condition:
		bool_branch = 1
	Utils.check(bool_branch == 1)

	var keep_running: bool = true
	var loop_count: int = 0
	while keep_running:
		loop_count += 1
		keep_running = false
	Utils.check(loop_count == 1)

	var and_result: bool = right_int < left_int and right_float < left_float
	var or_result: bool = left_int < right_int or left_float > right_float
	var ternary_result: int = 10 if right_int < left_int else 20
	Utils.check(and_result)
	Utils.check(or_result)
	Utils.check(ternary_result == 10)

	var nan: float = NAN
	var nan_less: bool = nan < 1.0
	Utils.check(not nan_less)
	var nan_branch: int = 0
	if nan < 1.0:
		nan_branch = 1
	else:
		nan_branch = 2
	Utils.check(nan_branch == 2)

	print("typed opcodes: ok")
