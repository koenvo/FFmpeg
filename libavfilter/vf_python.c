/*
 * Copyright (c) 2019 Koen Vossen
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Add Python video filter
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "libavformat/avio.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"

typedef struct PythonContext {
    const AVClass *class;

    char              *module;
    char              *init_function;
    char              *init_args;

    char              *filter_function;
    char              *uninit_function;

    PyObject          *pModule;
    PyObject          *pFunc;
} PythonContext;

#define OFFSET(x) offsetof(PythonContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption python_options[] = {
    { "module",               "Python module",                     OFFSET(module),          AV_OPT_TYPE_STRING, { .str = NULL }, 0, 1, FLAGS },
    { "init_function",        "Initialization function name",      OFFSET(init_function),   AV_OPT_TYPE_STRING, { .str = NULL }, 0, 1, FLAGS },
    { "init_args",            "Argument string for init function", OFFSET(init_args),       AV_OPT_TYPE_STRING, { .str = NULL }, 0, 1, FLAGS },

    { "filter_function",      "Filter function name",              OFFSET(filter_function), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 1, FLAGS },
    { "uninit_function",      "Uninitialization function name",    OFFSET(uninit_function), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 1, FLAGS },

    { NULL }
};

AVFILTER_DEFINE_CLASS(python);

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    const enum AVPixelFormat pixel_fmts[] = {
        AV_PIX_FMT_RGB24,
        AV_PIX_FMT_NONE
    };

    formats = ff_make_format_list(pixel_fmts);

    return ff_set_common_formats(ctx, formats);
}

static int config_inputs(AVFilterLink *inlink)
{
    AVFilterContext *ctx          = inlink->dst;

    // Consider configuring something like this:
    /*
    DRContext *dr_context         = ctx->priv;
    const char *model_output_name = "y";
    DNNReturnType result;

    dr_context->input.width    = inlink->w;
    dr_context->input.height   = inlink->h;
    dr_context->input.channels = 3;

    result = (dr_context->model->set_input_output)(dr_context->model->model, &dr_context->input, "x", &model_output_name, 1);
    if (result != DNN_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "could not set input and output for the model\n");
        return AVERROR(EIO);
    } */

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx  = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    PythonContext *py_context = ctx->priv;
    PyObject *pArgs, *pValue;

    av_frame_make_writable(in);

    pArgs = PyTuple_New(4);

    pValue = PyFloat_FromDouble(in->pts * av_q2d(inlink->time_base));
    PyTuple_SetItem(pArgs, 0, pValue);

    pValue = PyLong_FromLong(in->width);
    PyTuple_SetItem(pArgs, 1, pValue);

    pValue = PyLong_FromLong(in->height);
    PyTuple_SetItem(pArgs, 2, pValue);

    pValue = PyLong_FromLong(in->data[0]);
    PyTuple_SetItem(pArgs, 3, pValue);

    pValue = PyObject_CallObject(py_context->pFunc, pArgs);
    Py_DECREF(pArgs);
    if (pValue != NULL) {
        Py_DECREF(pValue);
    }

    return ff_filter_frame(outlink, in);
}

static int check_and_call_init(PythonContext *py_context)
{
    PyObject *pFunc, *pValue, *pArgs;

    if (py_context->init_function != NULL)
    {
        pFunc = PyObject_GetAttrString(py_context->pModule, py_context->init_function);
        /* pFunc is a new reference */

        if (pFunc && PyCallable_Check(pFunc)) {
            pArgs = PyTuple_New(1);

            pValue = PyUnicode_DecodeFSDefault(py_context->init_args);
            PyTuple_SetItem(pArgs, 0, pValue);

            pValue = PyObject_CallObject(pFunc, pArgs);
            Py_DECREF(pArgs);
            Py_XDECREF(pValue);
            Py_XDECREF(pFunc);
        } else {
            Py_XDECREF(pFunc);
            return -1;
        }
    }
    return 0;
}

static int check_and_call_uninit(PythonContext *py_context)
{
    PyObject *pFunc, *pValue, *pArgs;

    if (py_context->uninit_function != NULL)
    {
        pFunc = PyObject_GetAttrString(py_context->pModule, py_context->uninit_function);
        /* pFunc is a new reference */

        if (pFunc && PyCallable_Check(pFunc)) {
            pArgs = PyTuple_New(0);
            pValue = PyObject_CallObject(pFunc, pArgs);
            Py_DECREF(pArgs);
            Py_XDECREF(pValue);
            Py_XDECREF(pFunc);
        } else {
            Py_XDECREF(pFunc);
            return -1;
        }
    }
    return 0;
}


static av_cold int init(AVFilterContext *ctx)
{
    PyObject *pName;
    PythonContext *py_context = ctx->priv;

    Py_Initialize();
    pName = PyUnicode_DecodeFSDefault(py_context->module);
    /* Error checking of pName left out */

    py_context->pModule = PyImport_Import(pName);
    Py_DECREF(pName);

    if (py_context->pModule != NULL) {
        py_context->pFunc = PyObject_GetAttrString(py_context->pModule, py_context->filter_function);
        /* pFunc is a new reference */

        if (py_context->pFunc == NULL || !PyCallable_Check(py_context->pFunc)) {
            av_log(ctx, AV_LOG_ERROR, "could not load filter function\n");
            return AVERROR(EINVAL);
        }

        if (check_and_call_init(py_context) < 0) {
            av_log(ctx, AV_LOG_ERROR, "could not call init function\n");
            return AVERROR(EINVAL);
        }
    } else {
        av_log(ctx, AV_LOG_ERROR, "could not load module\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    PythonContext *py_context = ctx->priv;

    if (check_and_call_uninit(py_context) < 0) {
        av_log(ctx, AV_LOG_ERROR, "could not call uninit function\n");
    }

    Py_DECREF(py_context->pFunc);
    Py_DECREF(py_context->pModule);

    Py_FinalizeEx();
}

static const AVFilterPad python_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_inputs,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad python_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_python = {
    .name          = "python",
    .description   = NULL_IF_CONFIG_SMALL("Apply python filter to the input."),
    .priv_size     = sizeof(PythonContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = python_inputs,
    .outputs       = python_outputs,
    .priv_class    = &python_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
