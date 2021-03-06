#include "Interpreter.h"
#include "../bytecode/Bytecode.h"
#include "../bytecode/Opcode.h"
#include "../runtime/NObject.h"
#include "../runtime/NyxVM.h"

Interpreter::Interpreter() { this->frame = nullptr; }

Interpreter::~Interpreter() {
    frame = nullptr;
    for (auto& val : stack) {
        delete val;
    }
}

void Interpreter::neg(NObject* object) {
    if (typeid(*object) == typeid(NInt)) {
        nyx::int32 val = -dynamic_cast<NInt*>(object)->value;
        frame->push(new NInt(val));
    } else if (typeid(*object) == typeid(NDouble)) {
        double val = -dynamic_cast<NDouble*>(object)->value;
        frame->push(new NDouble(val));
    } else {
        panic("should not reach here");
    }
}

bool Interpreter::deepCompare(int cond, NObject* o1, NObject* o2) {
    if (o1 == nullptr || o2 == nullptr) {
        return cond == Opcode::TEST_EQ == (o1 == nullptr && o2 == nullptr);
    }

    if (typeid(*o1) == typeid(NInt) && typeid(*o2) == typeid(NInt)) {
        auto* t1 = dynamic_cast<NInt*>(o1);
        auto* t2 = dynamic_cast<NInt*>(o2);
        return genericCompare(cond, t1, t2);
    } else if (typeid(*o1) == typeid(NDouble) &&
               typeid(*o2) == typeid(NDouble)) {
        auto* t1 = dynamic_cast<NDouble*>(o1);
        auto* t2 = dynamic_cast<NDouble*>(o2);
        return genericCompare(cond, t1, t2);
    } else if (typeid(*o1) == typeid(NChar) && typeid(*o2) == typeid(NChar)) {
        auto* t1 = dynamic_cast<NChar*>(o1);
        auto* t2 = dynamic_cast<NChar*>(o2);
        return genericCompare(cond, t1, t2);
    } else if (typeid(*o1) == typeid(NString) &&
               typeid(*o2) == typeid(NString)) {
        auto* t1 = dynamic_cast<NString*>(o1);
        auto* t2 = dynamic_cast<NString*>(o2);
        return genericCompare(cond, t1, t2);
    } else if (typeid(*o1) == typeid(NArray) && typeid(*o2) == typeid(NArray)) {
        assert(cond == Opcode::TEST_EQ || cond == Opcode::TEST_NE);
        auto* t1 = dynamic_cast<NArray*>(o1);
        auto* t2 = dynamic_cast<NArray*>(o2);
        if (cond == Opcode::TEST_EQ) {
            if (t1->length != t2->length) {
                return false;
            }
            for (int i = 0; i < t1->length; i++) {
                if (!deepCompare(Opcode::TEST_EQ, t1->array[i], t2->array[i])) {
                    return false;
                }
            }

            return true;
        } else {
            if (t1->length != t2->length) {
                return true;
            }
            for (int i = 0; i < t1->length; i++) {
                if (!deepCompare(Opcode::TEST_EQ, t1->array[i], t2->array[i])) {
                    return true;
                }
            }
            return false;
        }

    } else {
        panic("should not reach here");
    }
}

void Interpreter::createFrame(Bytecode* bytecode, int argc, NObject** argv) {
    // Create execution frame
    this->frame = new Frame(bytecode->localVars.size());
    for (int i = 0; i < argc; i++) {
        auto* obj = argv[i];
        frame->store(i, obj);
    }

    if (!bytecode->freeVars.empty()) {
        for (auto* freeVar : bytecode->freeVars) {
            if (freeVar->isEnclosing) {
                freeVar->value.active = &frame->local()[freeVar->varIndex];
            }
        }
    }

    stack.push_back(frame);
}

void Interpreter::destroyFrame(Bytecode* bytecode, bool hasReturnValue) {
    // Destroy current frame, current frame may points to next frame
    auto* temp = stack.back();
    NObject* returnVale = nullptr;
    if (hasReturnValue) {
        returnVale = temp->pop();
    }
    stack.pop_back();
    if (!stack.empty()) {
        this->frame = stack.back();
        if (returnVale != nullptr) {
            this->frame->push(returnVale);
        }
    } else {
        this->frame = nullptr;
    }

    if (!bytecode->freeVars.empty()) {
        // If this is the enclosing frame, we must "move" free variable's value
        // to its endpoint. After that we should set local slot to null in order
        // to avoid releasing free variable's value
        // Note that we must separate "move" and set actions, since multi
        // endpoints may refer to the same free variable's value, if we move
        // value and immediately set to null, other endpoints will refer to null
        // value.
        for (auto* freeVar : bytecode->freeVars) {
            if (freeVar->isEnclosing) {
                freeVar->endpoint->value.inactive = (*(freeVar->value.active));
            }
        }

        for (auto* freeVar : bytecode->freeVars) {
            if (freeVar->isEnclosing) {
                temp->local()[freeVar->varIndex] = nullptr;
            }
        }
    }
    delete temp;
}

void Interpreter::loadFreeVar(FreeVar* freeVar) {
    if (freeVar->value.inactive != nullptr) {
        // Enclosing context is inactive, current context owns the
        // free variable
        frame->push(freeVar->value.inactive);
    } else {
        // Enclosing context is still active, current context just
        // "borrows" free variable
        frame->push(*(freeVar->endpoint->value.active));
    }
}

void Interpreter::storeFreeVar(FreeVar* freeVar, NObject* object) {
    if (freeVar->value.inactive != nullptr) {
        // Enclosing context is inactive, current context owns the
        // free variable
        freeVar->value.inactive = object;
    } else {
        // Enclosing context is still active, current context just
        // "borrows" free variable
        *(freeVar->endpoint->value.active) = object;
    }
}

void Interpreter::call(Bytecode* bytecode, int bci) {
    int funcArgc = bytecode->code[bci + 1];

    auto** funcArgv = new NObject*[funcArgc];
    for (int k = funcArgc - 1; k >= 0; k--) {
        funcArgv[k] = frame->pop();
    }

    NObject* callee = frame->pop();
    if (typeid(*callee) != typeid(NCallable)) {
        panic("callee '%s' is not callable");
    }
    auto* callable = dynamic_cast<NCallable*>(callee);
    if (callable->isNative) {
        NObject* result = ((NObject * (*)(int, NObject**))(
            (const char*)callable->code.native))(funcArgc, funcArgv);
        frame->push(result);
        return;
    } else {
        this->execute(callable->code.bytecode, funcArgc, funcArgv);
        return;
    }
    // TODO release funcArgv
}

void Interpreter::execute(Bytecode* bytecode, int argc, NObject** argv) {
    createFrame(bytecode, argc, argv);
    auto* code = bytecode->code;
    // Execute bytecode
    {
        for (int bci = 0; bci < bytecode->codeSize; bci++) {
            switch (code[bci]) {
            case Opcode::CALL: {
                call(bytecode, bci);
                bci += 1;
                break;
            }
            case Opcode::CONST_I: {
                nyx::int32 value = *(nyx::int32*)(code + bci + 1);
                auto* object = new NInt(value);
                frame->push(object);
                bci += 4;
                break;
            }
            case Opcode::CONST_D: {
                double value = *(double*)(code + bci + 1);
                auto* object = new NDouble(value);
                frame->push(object);
                bci += 8;
                break;
            }
            case Opcode::CONST_C: {
                nyx::int8 value = *(nyx::int8*)(code + bci + 1);
                auto* object = new NChar(value);
                frame->push(object);
                bci += 1;
                break;
            }
            case Opcode::CONST_NULL: {
                frame->push(nullptr);
                break;
            }
            case Opcode::CONST_STR: {
                int index = code[bci + 1];
                auto& str = bytecode->strings[index];
                NObject* object = new NString(str);
                frame->push(object);
                bci++;
                break;
            }
            case Opcode::ADD: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                arithmetic<Opcode::ADD>(object1, object2);
                break;
            }
            case Opcode::SUB: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                arithmetic<Opcode::SUB>(object1, object2);
                break;
            }
            case Opcode::MUL: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                arithmetic<Opcode::MUL>(object1, object2);
                break;
            }
            case Opcode::DIV: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                arithmetic<Opcode::DIV>(object1, object2);
                break;
            }
            case Opcode::REM: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                arithmetic<Opcode::REM>(object1, object2);
                break;
            }
            case Opcode::TEST_EQ: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                compare<Opcode::TEST_EQ>(object1, object2);
                break;
            }
            case Opcode::TEST_NE: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                compare<Opcode::TEST_NE>(object1, object2);
                break;
            }
            case Opcode::TEST_GE: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                compare<Opcode::TEST_GE>(object1, object2);
                break;
            }
            case Opcode::TEST_GT: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                compare<Opcode::TEST_GT>(object1, object2);
                break;
            }
            case Opcode::TEST_LE: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                compare<Opcode::TEST_LE>(object1, object2);
                break;
            }
            case Opcode::TEST_LT: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                compare<Opcode::TEST_LT>(object1, object2);
                break;
            }
            case Opcode::JMP: {
                int target = code[bci + 1];
                bci = target - 1;
                break;
            }
            case Opcode::JMP_NE: {
                auto* cond = dynamic_cast<NInt*>(frame->pop());
                assert(cond->value == 0 || cond->value == 1);
                if (cond->value == 0) {
                    // Goto target
                    int target = code[bci + 1];
                    bci = target - 1;
                } else {
                    // Otherwise, just forward bci
                    bci++;
                }
                break;
            }
            case Opcode::JMP_EQ: {
                auto* cond = dynamic_cast<NInt*>(frame->pop());
                assert(cond->value == 0 || cond->value == 1);
                if (cond->value == 1) {
                    // Goto target
                    int target = code[bci + 1];
                    bci = target - 1;
                } else {
                    // Otherwise, just forward bci
                    bci++;
                }
                break;
            }
            case Opcode::AND: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                bitop<Opcode::AND>(object1, object2);
                break;
            }
            case Opcode::OR: {
                NObject* object2 = frame->pop();
                NObject* object1 = frame->pop();
                bitop<Opcode::OR>(object1, object2);
                break;
            }
            case Opcode::NOT: {
                NObject* object1 = frame->pop();
                bitop<Opcode::NOT>(object1, nullptr);
                break;
            }
            case Opcode::NEG: {
                NObject* object = frame->pop();
                neg(object);
                break;
            }
            case Opcode::LOAD: {
                int index = code[bci + 1];
                frame->load(index);
                bci++;
                break;
            }
            case Opcode::STORE: {
                NObject* value = frame->pop();
                int index = code[bci + 1];
                frame->store(index, value);
                bci++;
                break;
            }
            case Opcode::LOAD_INDEX: {
                auto* index = dynamic_cast<NInt*>(frame->pop());
                auto* array = dynamic_cast<NArray*>(frame->pop());
                auto* elem = array->array[index->value];
                frame->push(elem);
                break;
            }
            case Opcode::STORE_INDEX: {
                auto* index = dynamic_cast<NInt*>(frame->pop());
                auto* object = frame->pop();
                auto* array = dynamic_cast<NArray*>(frame->pop());
                array->array[index->value] = object;
                break;
            }
            case Opcode::NEW_ARR: {
                int length = code[bci + 1];
                frame->push(new NArray(length));
                bci++;
                break;
            }
            case Opcode::DUP: {
                frame->dup();
                break;
            }
            case Opcode::RETURN: {
                destroyFrame(bytecode, false);
                return;
            }
            case Opcode::RETURN_VAL: {
                destroyFrame(bytecode, true);
                return;
            }
            case Opcode::ARR_LEN: {
                auto* array = frame->pop();
                if (typeid(*array) != typeid(NArray)) {
                    panic("array length is only operated on Array type");
                }
                int length = dynamic_cast<NArray*>(array)->length;
                frame->push(new NInt(length));
                break;
            }
            case Opcode::CONST_CALLABLE: {
                int callableIndex = code[bci + 1];
                if (callableIndex >= 0) {
                    Bytecode* temp = bytecode;
                    while (temp != nullptr) {
                        if (temp->callables.find(callableIndex) !=
                            temp->callables.end()) {
                            frame->push(new NCallable(
                                temp->callables[callableIndex], false));
                            break;
                        }
                        temp = temp->parent;
                    }
                } else {
                    frame->push(new NCallable(
                        bytecode->builtin[-callableIndex - 1][1], true));
                }
                bci++;
                break;
            }
            case Opcode::LOAD_FREE: {
                int freeIndex = code[bci + 1];
                FreeVar* freeVar = bytecode->freeVars[freeIndex];
                loadFreeVar(freeVar);
                bci++;
                break;
            }
            case Opcode::STORE_FREE: {
                auto* object = frame->pop();
                int freeIndex = code[bci + 1];
                FreeVar* freeVar = bytecode->freeVars[freeIndex];
                storeFreeVar(freeVar, object);
                bci++;
                break;
            }
            default:
                panic("invalid bytecode %d", code[bci]);
            }
        }
    }
    destroyFrame(bytecode, false);
}

void Interpreter::execute(Bytecode* bytecode) {
    PhaseTime timer("execute bytecodes via c++ interpreter");
    execute(bytecode, 0, nullptr);
}