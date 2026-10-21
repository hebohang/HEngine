#pragma once

#include "Runtime/EcsFramework/Entity/Entity.h"

#include <pybind11/pybind11.h>

#include <functional>
#include <string>
#include <utility>

namespace HEngine
{
    namespace py = pybind11;

    // A legacy field view resolves its path for every access. It never retains
    // the address of an ECS component, nested vector or blackboard.
    class LegacyReference
    {
    public:
        explicit LegacyReference(std::function<py::object()> resolve) : mResolve(std::move(resolve)) {}
        py::object Resolve() const { return mResolve(); }

        static py::object Unwrap(py::handle value)
        {
            if (py::isinstance<LegacyReference>(value)) return value.cast<const LegacyReference&>().Resolve();
            return py::reinterpret_borrow<py::object>(value);
        }

        static py::object View(std::function<py::object()> resolve)
        {
            py::object value = resolve();
            if (value.is_none() || PyBool_Check(value.ptr()) || PyLong_Check(value.ptr()) ||
                PyFloat_Check(value.ptr()) || PyUnicode_Check(value.ptr()) || PyBytes_Check(value.ptr()) || PyType_Check(value.ptr()) ||
                py::hasattr(py::type::of(value), "__members__"))
                return value;
            return py::cast(LegacyReference(std::move(resolve)));
        }

        py::object Attribute(const std::string& name) const
        {
            auto resolve = [self = *this, name] { return self.Resolve().attr(name.c_str()).cast<py::object>(); };
            py::object value = resolve();
            if (PyCallable_Check(value.ptr()) && !PyType_Check(value.ptr()))
            {
                return py::cpp_function([self = *this, name](py::args args, py::kwargs kwargs) -> py::object {
                    py::tuple arguments(args.size());
                    for (size_t i = 0; i < args.size(); ++i) arguments[i] = Unwrap(args[i]);
                    py::dict keywords;
                    for (auto item : kwargs) keywords[item.first] = Unwrap(item.second);
                    const auto call = [&] {
                        // Materialize the bound method before the temporary
                        // receiver is released; an attribute_accessor only borrows it.
                        py::object function = self.Resolve().attr(name.c_str());
                        auto* result = PyObject_Call(function.ptr(), arguments.ptr(), keywords.ptr());
                        if (!result) throw py::error_already_set();
                        return py::reinterpret_steal<py::object>(result);
                    };
                    // This existing getter exposes an embedded object; keep it a
                    // guarded view. Other legacy component methods return values.
                    if (name == "get_blackboard" && args.empty() && kwargs.empty())
                        return View([self, name] { return self.Resolve().attr(name.c_str())(); });
                    return call();
                });
            }
            return View(std::move(resolve));
        }

    private:
        std::function<py::object()> mResolve;
    };

    template<class T> py::object LegacyComponentReference(Entity entity)
    {
        if (!entity || !entity.HasComponent<T>()) throw py::value_error("Entity is invalid or the requested component is missing.");
        const auto lifetime = entity.GetComponent<T>().ReferenceLifetime();
        return py::cast(LegacyReference([entity, lifetime]() mutable -> py::object {
            const auto fail = [] {
                PyErr_SetString(PyExc_ReferenceError, "The entity, scene or component referenced by this Python view has been destroyed or replaced.");
                throw py::error_already_set();
            };
            if (lifetime.expired() || !entity || !entity.HasComponent<T>()) fail();
            const auto current = entity.GetComponent<T>().ReferenceLifetime();
            if (current.owner_before(lifetime) || lifetime.owner_before(current))
            {
                fail();
            }
            return py::cast(&entity.GetComponent<T>(), py::return_value_policy::reference);
        }));
    }

    inline void BindLegacyReference(py::module_& module)
    {
        py::class_<LegacyReference>(module, "_ComponentView")
            .def("__getattr__", &LegacyReference::Attribute)
            .def("__setattr__", [](const LegacyReference& self, const std::string& name, py::handle value) {
                py::setattr(self.Resolve(), name.c_str(), LegacyReference::Unwrap(value));
            })
            .def("__getitem__", [](const LegacyReference& self, py::object key) {
                return LegacyReference::View([self, key] { return self.Resolve()[key].cast<py::object>(); });
            })
            .def("__setitem__", [](const LegacyReference& self, py::object key, py::handle value) {
                self.Resolve()[key] = LegacyReference::Unwrap(value);
            })
            .def("__len__", [](const LegacyReference& self) { return py::len(self.Resolve()); })
            .def("__iter__", [](const LegacyReference& self) {
                py::list values;
                for (size_t i = 0; i < py::len(self.Resolve()); ++i)
                    values.append(LegacyReference::View([self, i] { return self.Resolve()[py::int_(i)].cast<py::object>(); }));
                return values.attr("__iter__")();
            })
            .def("__repr__", [](const LegacyReference& self) { return py::repr(self.Resolve()); });
    }
}
