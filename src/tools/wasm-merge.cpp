/*
 * Copyright 2017 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// A WebAssembly merger: loads multiple files, smashes them together,
// and emits the result.
//
// This is *not* a real linker. It just does naive merging.
//

#include <memory>

#include "pass.h"
#include "support/command-line.h"
#include "support/file.h"
#include "wasm-io.h"
#include "wasm-builder.h"

using namespace wasm;

// utilities

Name getNonColliding(Name initial, std::function<bool (Name)> checkIfCollides) {
  if (!checkIfCollides(initial)) {
    return initial;
  }
  int x = 0;
  while (1) {
    auto curr = Name(std::string(initial.str) + '$' + std::to_string(x));
    if (!checkIfCollides(curr)) {
      return curr;
    }
    x++;
  }
}

// gets the relative offset, assuming the form (add (global) (const offset)) or just (global) which has offset 0
bool getRelativeOffset(Expression* curr) {
  if (auto* binary = curr->dynCast<Binary>()) {
    assert(binary->op == AddInt32;
    assert(binary->left->is<GetGlobal>());
    return binary->right->cast<Const>()->value.geti32();
  }
  assert(curr->is<GetGlobal>());
  return 0; // no offset
}

// Merges input into output.
// May destructively modify input, we don't expect to use it later
// We don't copy into the new module, we assume both stay alive forever
void mergeIn(Module& output, Module& input) {
  // we will need to update names
  struct Updater : public ExpressionStackWalker<Updater, Visitor<Updater>> {
    // mappings, old name => new name
    std::unordered_map<Name, Name> ftNames; // function types
    std::unordered_map<Name, Name> iNames; // imports
    std::unordered_map<Name, Name> eNames; // exports
    std::unordered_map<Name, Name> fNames; // functions
    std::unordered_map<Name, Name> gNames; // globals

    // memory and table base are imported into these globals
    std::unordered_set<Name> memoryBaseGlobals,
                             tableBaseGlobals;

    // memory base and table base bumps
    Index memoryBaseBump,
          tableBaseBump;

    void visitCall(Call* curr) {
      curr->target = fNames[curr->target];
    }

    void visitCallImport(CallImport* curr) {
      curr->target = iNames[curr->target];
    }

    void visitCallIndirect(CallIndirect* curr) {
      curr->fullType = ftNames[curr->fullType];
    }

    void visitGetGlobal(GetGlobal* curr) {
      curr->name = gNames[curr->name];
      // if this is the memory or table base, add the bump
      if (memoryBaseGlobals.count(curr->name)) {
        addBump(memoryBaseBump);
      } else if (tableBaseGlobals.count(curr->name)) {
        addBump(tableBaseBump);
      }
    }

    void visitSetGlobal(SetGlobal* curr) {
      curr->name = gNames[curr->name];
    }

  private:
    // add an offset to a get_global. we look above, and if there is already an add,
    // we can add into it, avoiding creating a new node
    void addBump(Index bump) {
      if (expressionStack.size() >= 2) {
        auto* parent = expressionStack[expressionStack.size() - 2];
        if (auto* binary = parent->dynCast<Binary>()) {
          if (binary->op == AddInt32) {
            if (auto* num = binary->right->dynCast<Const>()) {
              num += bump;
              return;
            }
          }
        }
      }
      Builder builder(*getModule());
      replaceCurrent(
        builder.makeBinary(
          AddInt32,
          expressionStack.back(),
          builder.makeConst(Literal(int32_t(bump)))
        )
      );
    }
  };
  Updater updater;

  // find new names
  for (auto& curr : input.functionTypes) {
    curr->name = updater.ftNames[curr->name] = getNonColliding(curr->name, [&](Name name) -> bool {
      return output.checkFunctionType(name);
    });
  }
  for (auto& curr : input.imports) {
    curr->name = updater.iNames[curr->name] = getNonColliding(curr->name, [&](Name name) -> bool {
      return output.checkImport(name);
    });
  }
  for (auto& curr : input.exports) {
    curr->name = updater.eNames[curr->name] = getNonColliding(curr->name, [&](Name name) -> bool {
      return output.checkExport(name);
    });
  }
  for (auto& curr : input.functions) {
    curr->name = updater.fNames[curr->name] = getNonColliding(curr->name, [&](Name name) -> bool {
      return output.checkFunction(name);
    });
  }
  for (auto& curr : input.globals) {
    curr->name = updater.gNames[curr->name] = getNonColliding(curr->name, [&](Name name) -> bool {
      return output.checkGlobal(name);
    });
  }

  // memory: we place the new memory segments at a higher position. after the existing ones.
  //         that means we need to update usage of gb, which we did earlier.
  // first, find the highest relative offset (to memoryBase)
  updater.memoryBaseBump = 0;
  for (auto& segment : input.memory.segments) {
    updater.memoryBaseBump = std::max(updater.memoryBaseBump, getRelativeOffset(segment.offset));
  }
  // align to 16 bytes
  updater.memoryBaseBump = (updater.memoryBaseBump + 15) & -16;

  // table
  updater.tableBaseBump = 0;
  for (auto& segment : input.table.segments) {
    updater.tableBaseBump = std::max(updater.tableBaseBump, getRelativeOffset(segment.offset));
  }
  // align to 16 bytes
  updater.tableBaseBump = (updater.tableBaseBump + 15) & -16;

  // find the memory/table base globals, so we know how to update them
  for (auto& curr : input.imports) {
    if (curr->module == ENV) {
      if (curr->base == MEMORY_BASE) {
        updater.memoryBaseGlobals.insert(curr->name);
      } else if (curr->base == TABLE_BASE) {
        updater.tableBaseGlobals.insert(curr->name);
      }
    }
  }

  // update the contents we are about to merge
  updater.walk(input);

  // copy in the data
  for (auto& curr : input.functionTypes) {
    output.addFunctionType(curr);
  }
  for (auto& curr : input.imports) {
    curr->functionType = ftNames[curr->functionType];
    output.addImport(curr);
  }
  for (auto& curr : input.exports) {
    output.addExport(curr);
  }
  for (auto& curr : input.functions) {
    curr->type = ftNames[curr->type];
    output.addFunction(curr);
  }
  for (auto& curr : input.globals) {
    output.addGlobal(curr);
  }
}

//
// main
//

int main(int argc, const char* argv[]) {
  std::vector<std::string> filenames;
  bool emitBinary = true;

  Options options("wasm-merge", "Merge wasm files");
  options
      .add("--output", "-o", "Output file",
           Options::Arguments::One,
           [](Options* o, const std::string& argument) {
             o->extra["output"] = argument;
             Colors::disable();
           })
      .add("--emit-text", "-S", "Emit text instead of binary for the output file",
           Options::Arguments::Zero,
           [&](Options *o, const std::string &argument) { emitBinary = false; })
      .add_positional("INFILES", Options::Arguments::N,
                      [&](Options *o, const std::string &argument) {
                        filenames.push_back(argument);
                      });

  Module output;
  std::vector<std::unique_ptr<Module>> otherModules; // keep all inputs alive, to save copies
  bool first = true;
  for (auto& filename : filenames) {
    ModuleReader reader;
    if (first) {
      // read the first right into output, don't waste time merging into an empty module
      try {
        reader.read(filename, output);
      } catch (ParseException& p) {
        p.dump(std::cerr);
        Fatal() << "error in parsing input";
      }
      first = false;
    } else {
      std::unique_ptr<Module> input = wasm::make_unique<Module>();
      try {
        reader.read(filename, input);
      } catch (ParseException& p) {
        p.dump(std::cerr);
        Fatal() << "error in parsing input";
      }
      mergeIn(output, *input);
      otherModules.push_back(input);
    }
  }

  if (!WasmValidator().validate(output)) {
    Fatal() << "error in validating output";
  }

  if (options.extra.count("output") > 0) {
    ModuleWriter writer;
    writer.setDebug(options.debug);
    writer.setBinary(emitBinary);
    writer.write(output, options.extra["output"]);
  }
}
