struct ProjectileState:
	var position: Vector3
	var velocity: Vector3
	var lifetime: float = 1.0

struct Wrapper:
	var state: ProjectileState
	var enabled: bool

func make_state(x: float) -> ProjectileState:
	return ProjectileState(Vector3(x, 2.0, 3.0), Vector3(4.0, 5.0, 6.0), 2.0)

func move_state(state: ProjectileState) -> ProjectileState:
	state.position.x += 5.0
	return state

func make_lifetime(value: int) -> ProjectileState:
	return ProjectileState(Vector3.ZERO, Vector3.ZERO, value)

func set_lifetime(state: ProjectileState, value: Variant) -> ProjectileState:
	state.lifetime = value
	return state

func box_state(state: ProjectileState) -> Variant:
	var boxed: Variant = state
	return boxed

func unbox_state(value: Variant) -> ProjectileState:
	var state: ProjectileState = value
	return state

func test():
	var original := make_state(1.0)
	var copy: ProjectileState = original
	copy.position.y = 10.0
	print(original.position)
	print(copy.position)
	print(original == copy)

	var same := make_state(1.0)
	print(original == same)
	print(is_same(original, same))
	print(move_state(original).position)
	print(original.position)

	var defaults: ProjectileState = ProjectileState()
	print(defaults.position)
	print(defaults.lifetime)
	print(make_lifetime(3).lifetime)
	print(set_lifetime(defaults, 4.0).lifetime)

	var wrapper := Wrapper(original, true)
	wrapper.state.velocity.z = 9.0
	print(wrapper.state.velocity)
	print(original.velocity)
	print(typeof(original) == TYPE_STRUCT)
	print(original is ProjectileState)
	var boxed: Variant = box_state(original)
	print(typeof(boxed) == TYPE_STRUCT)
	print(unbox_state(boxed) == original)
