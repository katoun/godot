def can_build(env, platform):
    env.module_add_dependencies("gdscript", ["jsonrpc", "websocket"], True)
    return True


def get_opts(platform):
    from SCons.Variables import BoolVariable

    jit_supported = platform in ["android", "linuxbsd", "macos", "windows"]
    return [
        BoolVariable(
            "gdscript_baseline_jit",
            "Enable the experimental GDScript baseline JIT compiler",
            jit_supported,
        ),
    ]


def configure(env):
    pass


def get_doc_classes():
    return [
        "@GDScript",
        "GDScript",
        "GDScriptLanguageProtocol",
        "GDScriptSyntaxHighlighter",
        "GDScriptTextDocument",
        "GDScriptWorkspace",
    ]


def get_doc_path():
    return "doc_classes"
