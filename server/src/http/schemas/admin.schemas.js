"use strict";

// Skema input zod untuk endpoint /admin/*.

const { z } = require("zod");

const optStr = z
  .string()
  .optional()
  .or(z.literal("").transform(() => undefined));

const addClassSchema = z.object({
  name: z.string().min(1).max(128),
  code: z.string().min(1).max(64),
  lecturer: optStr,
  lecturer_user_id: z
    .union([z.literal(""), z.coerce.number().int()])
    .optional(),
  current_class: optStr,
});

const editClassSchema = addClassSchema.extend({
  id: z.coerce.number().int().positive(),
});

const idAndClassSchema = z.object({
  id: z.coerce.number().int().positive(),
  current_class: optStr,
});

const deleteClassSchema = idAndClassSchema;
const deleteStudentSchema = idAndClassSchema;

const editStudentSchema = z.object({
  id: z.coerce.number().int().positive(),
  name: z.string().min(1),
  nim: z.string().min(1).max(64),
  rfid: z.string().min(1).max(64),
  current_class: optStr,
});

const addStudentSchema = z.object({
  name: z.string().min(1),
  nim: z.string().min(1).max(64),
  rfid: z.string().min(1).max(64),
  kelas: optStr,
  current_class: optStr,
});

const addToClassSchema = z.object({
  student_id: z.coerce.number().int().positive(),
  current_class: z.string().min(1),
});

const addUserSchema = z.object({
  username: z.string().min(1).max(64),
  // Password minimal 8 karakter (sesuai standar dasar).
  password: z.string().min(8),
  role: z.enum(["admin", "dosen"]),
  current_class: optStr,
});

const deleteUserSchema = z.object({
  id: z.coerce.number().int(),
  current_class: optStr,
});

const manageUidSchema = z.object({
  action: z.enum(["delete", "delete_all"]),
  uid: optStr,
  entry_id: z.union([z.literal(""), z.coerce.number().int()]).optional(),
  current_class: optStr,
});

module.exports = {
  addClassSchema,
  editClassSchema,
  deleteClassSchema,
  deleteStudentSchema,
  editStudentSchema,
  addStudentSchema,
  addToClassSchema,
  addUserSchema,
  deleteUserSchema,
  manageUidSchema,
};
