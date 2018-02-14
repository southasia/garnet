// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package ir

import (
	"log"

	"fidl/compiler/backend/common"
	"fidl/compiler/backend/types"
)

// Type represents a golang type.
type Type string

// Struct represents a golang struct.
type Struct struct {
	// Name is the name of the golang struct.
	Name string

	// Members is a list of the golang struct members.
	Members []StructMember
}

// StructMember represents the member of a golang struct.
type StructMember struct {
	// Type is the type of the golang struct.
	Type Type
	Name string
}

// Root is the root of the golang backend IR structure.
//
// The golang backend IR structure is loosely modeled after an abstract syntax
// tree, and is used to generate golang code from templates.
type Root struct {
	// TODO(mknyszek): Support enums, unions, interfaces, and constants.

	// Structs represents the list of structs
	Structs []Struct
}

// Contains the full set of reserved golang keywords, in addition to a set of
// primitive named types. Note that this will result in potentially unnecessary
// identifier renaming, but this isn't a big deal for generated code.
var reservedWords = map[string]bool{
	// Officially reserved keywords.
	"break":       true,
	"case":        true,
	"chan":        true,
	"const":       true,
	"continue":    true,
	"default":     true,
	"defer":       true,
	"else":        true,
	"fallthrough": true,
	"for":         true,
	"func":        true,
	"go":          true,
	"goto":        true,
	"if":          true,
	"int":         true,
	"interface":   true,
	"map":         true,
	"package":     true,
	"range":       true,
	"return":      true,
	"select":      true,
	"struct":      true,
	"switch":      true,
	"try":         true,
	"type":        true,
	"var":         true,

	// Reserved types.
	"bool": true,
	"byte": true,
	"int8": true,
	"int16": true,
	"int32": true,
	"int64": true,
	"rune": true,
	"string": true,
	"uint8": true,
	"uint16": true,
	"uint32": true,
	"uint64": true,

	// Reserved values.
	"false": true,
	"true": true,
}

var primitiveTypes = map[types.PrimitiveSubtype]string{
	types.Bool:    "bool",
	types.Status:  "int32",
	types.Int8:    "int8",
	types.Int16:   "int16",
	types.Int32:   "int32",
	types.Int64:   "int64",
	types.Uint8:   "uint8",
	types.Uint16:  "uint16",
	types.Uint32:  "uint32",
	types.Uint64:  "uint64",
	types.Float32: "float32",
	types.Float64: "float64",
}

func exportIdentifier(name types.Identifier) types.Identifier {
	return types.Identifier(common.ToCamelCase(string(name)))
}

func isReservedWord(str string) bool {
	_, ok := reservedWords[str]
	return ok
}

func changeIfReserved(val types.Identifier) string {
	// TODO(mknyszek): Detect name collision within a scope as a result of transforming.
	str := string(val)
	if isReservedWord(str) {
		return str + "_"
	}
	return str
}

func compilePrimitiveSubtype(val types.PrimitiveSubtype) Type {
	t, ok := primitiveTypes[val]
	if !ok {
		log.Fatal("Unknown primitive type:", val)
	}
	return Type(t)
}

func compileType(val types.Type) Type {
	var r Type
	// TODO(mknyszek): Support arrays, vectors, strings, handles, requests,
	// and identifiers.
	switch val.Kind {
	case types.PrimitiveType:
		r = compilePrimitiveSubtype(val.PrimitiveSubtype)
	default:
		log.Fatal("Unknown type kind:", val.Kind)
	}
	return r
}

func compileStructMember(val types.StructMember) StructMember {
	return StructMember{
		Type: compileType(val.Type),
		Name: changeIfReserved(exportIdentifier(val.Name)),
	}
}

func compileStruct(val types.Struct) Struct {
	r := Struct{
		Name: changeIfReserved(exportIdentifier(val.Name)),
	}
	for _, v := range val.Members {
		r.Members = append(r.Members, compileStructMember(v))
	}
	return r
}

// Compile translates parsed FIDL IR into golang backend IR for code generation.
func Compile(fidlData types.Root) Root {
	r := Root{}
	for _, v := range fidlData.Structs {
		r.Structs = append(r.Structs, compileStruct(v))
	}
	return r
}
