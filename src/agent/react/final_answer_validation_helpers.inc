namespace {

bool side_effect_level_is_read_only(std::string value) {
  value = lower_ascii(trim_copy(std::move(value)));
  return value == "none" || value == "read-only" || value == "readonly" || value == "read_only";
}

bool tool_name_looks_read_only(const std::string& name) {
  const auto text = lower_ascii(name);
  return contains_any(text, {
      "read", "list", "search", "fetch", "get", "query", "lookup", "inspect",
      "describe", "eval", "retrieve", "find", "stat", "exists",
  });
}

bool tool_name_looks_side_effecting(const std::string& name) {
  const auto text = lower_ascii(name);
  return contains_any(text, {
      "write", "create", "edit", "update", "delete", "remove", "rename", "move",
      "patch", "run", "exec", "install", "build", "test", "open", "click", "type",
      "upload", "download", "commit", "apply", "save", "browser", "screen", "system",
      "terminal", "shell",
  });
}

bool tool_definition_is_side_effecting(const ToolDefinition* tool, const ToolCall& call) {
  if (!tool) {
    return !tool_name_looks_read_only(call.name);
  }
  if (tool->mutates_files || tool->interactive || tool->long_running) {
    return true;
  }
  if (tool_name_looks_side_effecting(tool->name)) {
    return true;
  }
  if (tool_text_contains_any(*tool, {"browser", "screen", "system", "terminal", "shell", "process.exec"})) {
    return true;
  }
  if (tool->read_only || side_effect_level_is_read_only(tool->side_effect_level)) {
    return false;
  }
  if (tool_name_looks_read_only(tool->name)) {
    return false;
  }
  return true;
}

bool has_successful_side_effect_tool_result(const FinalAnswerValidationInput& input) {
  for (const auto& result : input.completed_tool_results) {
    if (!result.ok) {
      continue;
    }
    if (tool_definition_is_side_effecting(find_tool_definition(input.tools, result.tool_call.name),
                                          result.tool_call)) {
      return true;
    }
  }
  return false;
}

bool final_answer_reports_blocker(const std::string& lower_answer) {
  return contains_any(lower_answer, {
      "cannot", "can't", "could not", "unable", "failed", "failure", "error",
      "permission", "denied", "blocked", "missing", "need more information",
      "need you to", "not enough", "insufficient", "not possible",
      "无法", "不能", "失败", "错误", "报错", "权限", "拒绝", "阻止", "阻塞",
      "缺少", "需要你", "需要更多", "没有足够", "找不到", "不存在",
  });
}

bool text_describes_future_work(const std::string& lower_text) {
  const bool has_future_marker = contains_any(lower_text, {
      "i will", "i'll", "i am going to", "i’m going to", "going to",
      "next i", "i need to", "need to use", "will use", "will create",
      "will run", "will execute", "will install", "will modify", "will write",
      "will open", "will click", "will generate", "will save",
      "我会", "我将", "将会", "接下来", "准备", "会创建", "会执行", "会运行",
      "会安装", "会修改", "会写入", "会使用工具", "需要使用工具", "我需要",
  });
  if (!has_future_marker) {
    return false;
  }
  return contains_any(lower_text, {
      "tool", "create", "write", "modify", "edit", "delete", "convert", "run",
      "execute", "install", "build", "test", "open", "click", "type", "download",
      "upload", "generate", "save", "patch", "commit", "browser", "screen",
      "system", "terminal", "shell",
      "工具", "创建", "新建", "写入", "保存", "修改", "编辑", "删除", "转换",
      "运行", "执行", "安装", "构建", "测试", "打开", "点击", "输入", "下载",
      "上传", "生成", "补丁", "提交", "浏览器", "屏幕", "系统", "终端",
  });
}

bool request_is_informational(const std::string& lower_request) {
  return contains_any(lower_request, {
      "how to", "how do i", "what is", "why", "explain", "describe", "should i",
      "is it", "whether", "如何", "怎么", "为什么", "解释", "说明", "是否",
      "属实", "值得", "吗", "？", "?",
  }) && !contains_any(lower_request, {
      "帮我", "给我", "替我", "开始", "落地", "修复一下", "实现一下",
      "please create", "please modify", "please run", "please install",
  });
}

bool request_requires_side_effect(const std::string& lower_request) {
  if (request_is_informational(lower_request)) {
    return false;
  }
  if (contains_any(lower_request, {
          "create file", "create a file", "create the file", "new file",
          "write file", "write a file", "write the file", "write to file",
          "save file", "save to file", "generate file", "generate a file",
          "make a file", "modify file", "edit file", "delete file",
          "remove file", "rename file", "move file", "patch file",
          "创建文件", "新建文件", "写入文件", "保存文件", "生成文件",
          "修改文件", "编辑文件", "删除文件", "移除文件", "重命名文件",
          "移动文件",
      })) {
    return true;
  }
  if (contains_any(lower_request, {
          "run command", "execute command", "run shell", "shell command",
          "terminal command", "terminal", "shell", "install dependency",
          "install dependencies", "install package", "npm ", "pnpm ", "yarn ",
          "cargo ", "cmake ", "make ", "ctest", "pytest", "go test",
          "运行命令", "执行命令", "终端", "shell", "安装依赖", "安装包",
      })) {
    return true;
  }
  if (contains_any(lower_request, {
          "open browser", "operate browser", "browser", "click", "type into",
          "screen", "system setting", "system state", "download", "upload",
          "打开浏览器", "操作浏览器", "浏览器", "点击", "输入到", "屏幕",
          "系统设置", "系统状态", "下载", "上传",
      })) {
    return true;
  }
  if (contains_any(lower_request, {
          "codebase", "source file", " repo ", "repository", "project file",
          ".cpp", ".hpp", ".cc", ".cxx", ".h", ".ts", ".tsx", ".js", ".jsx",
          ".py", ".go", ".rs", ".java", ".kt", ".swift", ".json", ".yaml",
          ".yml", ".md",
          "代码库", "源码文件", "仓库", "项目文件",
      }) &&
      contains_any(lower_request, {
          "modify", "edit", "delete", "remove", "rename", "move", "convert",
          "patch", "commit", "fix", "implement", "refactor", "compile",
          "build", "test",
          "修改", "编辑", "删除", "移除", "重命名", "移动", "转换", "补丁",
          "提交", "修复", "实现", "落地", "重构", "编译", "构建", "测试",
      })) {
    return true;
  }
  if (contains_any(lower_request, {
          "apply patch", "commit changes", "build project", "run tests",
          "run test", "compile project", "fix bug", "fix issue",
          "implement feature", "refactor code", "modify code", "edit code",
          "change code", "update code",
          "应用补丁", "提交代码", "构建项目", "运行测试", "编译项目",
          "修复 bug", "修复问题", "实现功能", "重构代码", "修改代码",
          "编辑代码", "更新代码",
      })) {
    return true;
  }
  return false;
}

}  // namespace
