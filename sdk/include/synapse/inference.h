/*
    inference.h: Вычисления с квантованными матрицами
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Вычисления с квантованными матрицами
    Подсистема: общий интерфейс ядра и приложений
    Первое изменение: 13.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef SYNAPSE_INFERENCE_H
#define SYNAPSE_INFERENCE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYN_INFER_FORCE_SCALAR 1u
#define SYN_INFER_BACKEND_SCALAR 0u
#define SYN_INFER_BACKEND_AVX2_FMA 1u

// Вычисления выполняются на CPU без выделения памяти и дополнительных потоков
// Функции возвращают 0 либо -SYN_EINVAL и -SYN_EOVERFLOW, errno не меняется
// Вернуть выбранный способ вычислений
unsigned syn_inference_backend(void);
// Умножить матрицу Q8 на вектор с масштабами последовательных групп
// Число столбцов кратно размеру группы от 1 до 1024, веса хранятся по строкам
// Массивы не пересекаются с output, входные значения и масштабы конечны
// Вызывающий код отвечает за размеры и срок жизни массивов
// SYN_INFER_FORCE_SCALAR позволяет явно выбрать скалярную проверку
int syn_q8_gemv(float *output, const int8_t *x, const float *x_scales, const int8_t *weights,
                const float *weight_scales, size_t columns, size_t rows, size_t group,
                unsigned flags);

#ifdef __cplusplus
}
#endif

#endif
