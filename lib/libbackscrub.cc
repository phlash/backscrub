/* This is licenced software, @see LICENSE file.
 * Authors - @see AUTHORS file.
==============================================================================*/

#include <unzip.h>
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"
#include "tensorflow/lite/optional_debug_tools.h"

#include "transpose_conv_bias.h"
#include "libbackscrub.h"

#include "metadata_schema_generated.h"

// Internal context structures
enum class modeltype_t {
	Unknown,
	BodyPix,
	DeepLab,
	GoogleMeetSegmentation,
	MLKitSelfie,
};

struct normalization_t {
	float scaling;
	float offset;
};

struct backscrub_ctx_t {
	// Loaded inference model
	std::unique_ptr<tflite::FlatBufferModel> model;
	// Model interpreter instance
	std::unique_ptr<tflite::Interpreter> interpreter;
	// Specific model type & input normalization
	modeltype_t modeltype;
	normalization_t norm;
	// Deeplab label info for post-processing
	std::vector<std::string> labels;
	size_t cnum;
	size_t pers;
	// Optional callbacks with caller-provided context
	void (*ondebug)(void *ctx, const char *msg);
	void (*onprep)(void *ctx);
	void (*oninfer)(void *ctx);
	void (*onmask)(void *ctx);
	void *caller_ctx;
	// Processing state
	cv::Mat input;
	cv::Mat output;
	cv::Rect roidim;
	cv::Mat mask;
	cv::Mat mroi;
	cv::Mat ofinal;
	cv::Size blur;
	cv::Mat in_u8_bgr;
	cv::Rect in_roidim;
	float ratio;
	float frameratio;
};

// Debug helper
#ifdef WIN32
// https://stackoverflow.com/questions/40159892/using-asprintf-on-windows
static int vasprintf(char **msgp, const char *fmt, va_list ap) {
	int len = _vscprintf(fmt, ap);
	if (len<=0)
		return len;
	*msgp = (char *)malloc(len+1);
	len = vsprintf_s(*msgp, len+1, fmt, ap);
	if (len<=0) {
		free(*msgp);
		return len;
	}
	return len;
}
#endif
static void _dbg(backscrub_ctx_t &ctx, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	char *msg;
	if (ctx.ondebug && vasprintf(&msg, fmt, ap)>0) {
		ctx.ondebug(ctx.caller_ctx, msg);
		free(msg);
	} else {
		vfprintf(stderr, fmt, ap);
	}
	va_end(ap);
}

static cv::Mat getTensorMat(backscrub_ctx_t &ctx, int tnum) {

	TfLiteType t_type = ctx.interpreter->tensor(tnum)->type;
	if (kTfLiteFloat32 != t_type) {
		_dbg(ctx,"error: tensor #%d: is not float32 type (%d)\n", tnum, t_type);
		return cv::Mat();
	}

	TfLiteIntArray* dims = ctx.interpreter->tensor(tnum)->dims;
	for (int i = 0; i < dims->size; i++)
		_dbg(ctx,"tensor #%d: %d\n",tnum,dims->data[i]);
	if (dims->data[0] != 1) {
		_dbg(ctx,"error: tensor #%d: is not single vector (%d)\n", tnum, dims->data[0]);
		return cv::Mat();
	}

	int h = dims->data[1];
	int w = dims->data[2];
	int c = dims->data[3];

	float* p_data = ctx.interpreter->typed_tensor<float>(tnum);
	if (nullptr == p_data) {
		_dbg(ctx,"error: tensor #%d: unable to obtain data pointer\n", tnum);
		return cv::Mat();
	}

	return cv::Mat(h,w,CV_32FC(c),p_data);
}

static std::vector<std::string> read_labels(const char *modelname, const char *labelfile) {
	std::vector<std::string> res;
	unzFile zip = unzOpen(modelname);
	if (!zip)
		goto done;
	if (UNZ_OK!=unzLocateFile(zip, labelfile, 2))
		goto done;
	unz_file_info info;
	if (UNZ_OK!=unzGetCurrentFileInfo(zip, &info, nullptr, 0, nullptr, 0, nullptr, 0))
		goto done;
	if (UNZ_OK!=unzOpenCurrentFile(zip))
		goto done;
	{
		int len = (int)info.uncompressed_size;
		char *buf = new char[len+1];
		if (len!=unzReadCurrentFile(zip, buf, len)) {
			delete buf;
			goto done;
		}
		buf[len]=0;
		std::istringstream str(buf);
		std::string s;
		while (std::getline(str, s))
			res.push_back(s);
		delete buf;
		unzCloseCurrentFile(zip);
	}
done:
	if (zip)
		unzClose(zip);
	return res;
}

static int parse_metadata(const uint8_t *buf, const uint32_t size, float *pmean, float *pstdd, std::string *plabels) {
	// check we have the right buffer
	if (!tflite::ModelMetadataBufferHasIdentifier(buf))
		return 1;
	const tflite::ModelMetadata *md = tflite::GetModelMetadata(buf);
	if (md->version()->str() != "v1")
		return -1;
	const flatbuffers::Vector<flatbuffers::Offset<tflite::SubGraphMetadata>> *subg = md->subgraph_metadata();
	if (subg->size()!=1)
		return -3;
	const flatbuffers::Vector<flatbuffers::Offset<tflite::TensorMetadata>> *itmd = subg->Get(0)->input_tensor_metadata();
	if (itmd->size()!=1)
		return -4;
	const flatbuffers::Vector<flatbuffers::Offset<tflite::ProcessUnit>> *pus = itmd->Get(0)->process_units();
	if (pus->size()!=1)
		return -5;
	const tflite::ProcessUnit *pu = pus->Get(0);
	if (pu->options_type()!=tflite::ProcessUnitOptions::NormalizationOptions)
		return -6;
	const tflite::NormalizationOptions *norm = pu->options_as_NormalizationOptions();
	if (norm->mean()->size()!=1)
		return -7;
	if (norm->std()->size()!=1)
		return -8;
	*pmean = norm->mean()->Get(0);
	*pstdd = norm->std()->Get(0);
	const flatbuffers::Vector<flatbuffers::Offset<tflite::TensorMetadata>> *otmd = subg->Get(0)->output_tensor_metadata();
	if (otmd && otmd->size()==1) {
		*plabels = otmd->Get(0)->associated_files()->Get(0)->name()->str();
	}
	return 0;
}

// Determine type of model (and thus post-processing required) from file name :=(
static modeltype_t get_modeltype(const char* modelname) {
	if (strstr(modelname, "body-pix")) {
		return modeltype_t::BodyPix;
	}
	else if (strstr(modelname, "deeplab")) {
		return modeltype_t::DeepLab;
	}
	else if (strstr(modelname, "segm_")) {
		return modeltype_t::GoogleMeetSegmentation;
	}
	else if (strstr(modelname, "selfie")) {
		return modeltype_t::MLKitSelfie;
	}
	return modeltype_t::Unknown;
}

static normalization_t get_normalization(modeltype_t type) {
	// Only used when model has no metadata with this in
	normalization_t rv = {0};
	switch (type) {
		case modeltype_t::DeepLab:
			rv.scaling = 1/127.5; rv.offset = -1;
			break;
		case modeltype_t::BodyPix:
		case modeltype_t::GoogleMeetSegmentation:
		case modeltype_t::MLKitSelfie:
		case modeltype_t::Unknown:
		default:
			rv.scaling = 1/255.0; rv.offset = 0;
			break;
	}
	return rv;
}

// deeplabv3 classes
// TODO: read from model metadata file
static const std::vector<std::string> labels = { "background", "aeroplane", "bicycle", "bird", "boat", "bottle", "bus", "car", "cat", "chair", "cow", "dining table", "dog", "horse", "motorbike", "person", "potted plant", "sheep", "sofa", "train", "tv" };
// label number of "person" for DeepLab v3+ model
static const size_t cnum = labels.size();
static const size_t pers = std::distance(labels.begin(), std::find(labels.begin(),labels.end(),"person"));

void *bs_maskgen_new(
	// Required parameters
	const char *modelname,
	size_t threads,
	size_t width,
	size_t height,
	// Optional (nullable) callbacks with caller-provided context
	// ..debug output
	void (*ondebug)(void *ctx, const char *msg),
	// ..after preparing video frame
	void (*onprep)(void *ctx),
	// ..after running inference
	void (*oninfer)(void *ctx),
	// ..after generating mask
	void (*onmask)(void *ctx),
	// ..the returned context
	void *caller_ctx
) {
	// Allocate context
	backscrub_ctx_t *pctx = new backscrub_ctx_t;
	// Take a reference so we can write tidy code with ctx.<x>
	backscrub_ctx_t &ctx = *pctx;
	// Save callbacks
	ctx.ondebug = ondebug;
	ctx.onprep = onprep;
	ctx.oninfer = oninfer;
	ctx.onmask = onmask;
	ctx.caller_ctx = caller_ctx;
	// Load model
	ctx.model = tflite::FlatBufferModel::BuildFromFile(modelname);
	if (!ctx.model) {
		_dbg(ctx, "error: unable to load model from file: '%s'.\n", modelname);
		bs_maskgen_delete(pctx);
		return nullptr;
	}
	// Determine model type and normalization values
	ctx.modeltype = get_modeltype(modelname);
	if (modeltype_t::Unknown == ctx.modeltype) {
		_dbg(ctx, "error: unknown model type '%s'.\n", modelname);
		bs_maskgen_delete(pctx);
		return nullptr;
	}
	auto model = ctx.model->GetModel();
	auto *md = model->metadata();
	float tmpmean = 0.0, tmpstdd = 0.0;
	std::string labfile;
	if (md) {
		for (uint32_t mid=0; mid < md->size(); ++mid) {
			const auto meta = md->Get(mid);
			_dbg(ctx, "model metadata: %s\n", meta->name()->c_str());
			if (meta->name()->str() != "TFLITE_METADATA")
				continue;
			// grab raw buffer and parse it..
			const flatbuffers::Vector<uint8_t> *pvec = model->buffers()->Get(meta->buffer())->data();
			int rv = parse_metadata(pvec->data(), pvec->size(), &tmpmean, &tmpstdd, &labfile);
			if (rv)
				_dbg(ctx, "error: unable to parse TfLite metadata: %d\n", rv);
		}
	}
	if (tmpstdd!=0.0) {
		// valid metadata found
		ctx.norm.scaling = 1/tmpstdd;
		ctx.norm.offset = -(tmpmean/tmpstdd);
		_dbg(ctx, "\tmetadata normalisation: scaling=%f offset=%f\n", ctx.norm.scaling, ctx.norm.offset);
	} else {
		// guess from model type..
		ctx.norm = get_normalization(ctx.modeltype);
	}
	// Set Deeplabv3 labels from metadata or use defaults
	ctx.labels = { "background", "aeroplane", "bicycle", "bird", "boat", "bottle", "bus", "car", "cat", "chair", "cow", "dining table", "dog", "horse", "motorbike", "person", "potted plant", "sheep", "sofa", "train", "tv" };
	ctx.cnum = labels.size();
	ctx.pers = std::distance(labels.begin(), std::find(labels.begin(),labels.end(),"person"));
	if (!labfile.empty()) {
		std::vector<std::string> tmplabs = read_labels(modelname, labfile.c_str());
		if (tmplabs.size() > 0) {
			ctx.labels = tmplabs;
			ctx.cnum = labels.size();
			ctx.pers = std::distance(labels.begin(), std::find(labels.begin(),labels.end(),"person"));
			for (size_t l=0; l<ctx.cnum; ++l)
				_dbg(ctx, "\tlabel(%lu): %s\n", l, ctx.labels[l].c_str());
			_dbg(ctx, "\tperson@%ld\n", ctx.pers);
		}
	}
	// Build the interpreter
	tflite::ops::builtin::BuiltinOpResolver resolver;
	// custom op for Google Meet network
	resolver.AddCustom("Convolution2DTransposeBias", mediapipe::tflite_operations::RegisterConvolution2DTransposeBias());
	tflite::InterpreterBuilder builder(*ctx.model, resolver);
	builder(&ctx.interpreter);
	if (!ctx.interpreter) {
		_dbg(ctx, "error: unable to build model interpreter\n");
		bs_maskgen_delete(pctx);
		return nullptr;
	}

	// Allocate tensor buffers.
	if (ctx.interpreter->AllocateTensors() != kTfLiteOk) {
		_dbg(ctx, "error: unable to allocate tensor buffers\n");
		bs_maskgen_delete(pctx);
		return nullptr;
	}

	// set interpreter params
	ctx.interpreter->SetNumThreads(threads);
	ctx.interpreter->SetAllowFp16PrecisionForFp32(true);

	// get input and output tensor as cv::Mat
	ctx.input = getTensorMat(ctx, ctx.interpreter->inputs ()[0]);
	ctx.output = getTensorMat(ctx, ctx.interpreter->outputs()[0]);
	if (ctx.input.empty() || ctx.output.empty()) {
		bs_maskgen_delete(pctx);
		return nullptr;
	}
	ctx.ratio = (float)ctx.input.rows/(float) ctx.input.cols;
	ctx.frameratio = (float)height/(float)width;

	// initialize mask and model-aspect ROI in center
	if (ctx.frameratio < ctx.ratio) {
		// if frame is wider than model, then use only the frame center
		ctx.roidim = cv::Rect((width-height/ctx.ratio)/2,0,height/ctx.ratio,height);
		ctx.in_roidim = cv::Rect(0, 0, ctx.input.cols, ctx.input.rows);
	} else {
		// if model is wider than the frame, center the frame in the model
		ctx.roidim = cv::Rect(0, 0, width, height);
		ctx.in_roidim = cv::Rect((ctx.input.cols-ctx.input.rows/ctx.frameratio)/2, 0, ctx.input.rows/ctx.frameratio,ctx.input.rows);
	}

	ctx.mask = cv::Mat::ones(height,width,CV_8UC1)*255;
	ctx.mroi = ctx.mask(ctx.roidim);

	ctx.in_u8_bgr = cv::Mat(ctx.input.rows, ctx.input.cols, CV_8UC3, cv::Scalar(0, 0, 0));

	// mask blurring size
	ctx.blur = cv::Size(5,5);

	// create Mat for small mask
	ctx.ofinal = cv::Mat(ctx.output.rows,ctx.output.cols,CV_8UC1);
	return pctx;
}

void bs_maskgen_delete(void *context) {
	if (!context)
		return;
	backscrub_ctx_t &ctx = *((backscrub_ctx_t *)context);
	// clear all mask data
	ctx.ofinal.deallocate();
	ctx.mask.deallocate();
	ctx.input.deallocate();
	ctx.output.deallocate();
	// drop interpreter (if present)
	if (ctx.interpreter != nullptr)
		ctx.interpreter.reset();
	// drop model (if present)
	if (ctx.model != nullptr)
		ctx.model.reset();
	delete &ctx;
}

bool bs_maskgen_process(void *context, cv::Mat &frame, cv::Mat &mask) {
	if (!context)
		return false;
	backscrub_ctx_t &ctx = *((backscrub_ctx_t *)context);

	// map ROI
	cv::Mat roi = frame(ctx.roidim);

	cv::Mat in_u8_rgb;
	cv::Mat in_roi = ctx.in_u8_bgr(ctx.in_roidim);
	cv::resize(roi,in_roi,ctx.in_roidim.size());
	cv::cvtColor(ctx.in_u8_bgr,in_u8_rgb,cv::COLOR_BGR2RGB);

	// TODO: can convert directly to float?

	// bilateral filter to reduce noise
	if (1) {
		cv::Mat filtered;
		cv::bilateralFilter(in_u8_rgb,filtered,5,100.0,100.0);
		in_u8_rgb = filtered;
	}

	// convert to float and normalize values expected by the model
	in_u8_rgb.convertTo(ctx.input,CV_32FC3,ctx.norm.scaling,ctx.norm.offset);
	if (ctx.onprep)
		ctx.onprep(ctx.caller_ctx);

	// Run inference
	if (ctx.interpreter->Invoke() != kTfLiteOk) {
		_dbg(ctx, "error: failed to interpret video frame\n");
		return false;
	}
	if (ctx.oninfer)
		ctx.oninfer(ctx.caller_ctx);

	float* tmp = (float*)ctx.output.data;
	uint8_t* out = (uint8_t*)ctx.ofinal.data;

	switch (ctx.modeltype) {
		case modeltype_t::DeepLab:
			// find class with maximum probability
			for (unsigned int n = 0; n < ctx.output.total(); n++) {
				float maxval = -10000; size_t maxpos = 0;
				for (size_t i = 0; i < cnum; i++) {
					if (tmp[n*cnum+i] > maxval) {
						maxval = tmp[n*cnum+i];
						maxpos = i;
					}
				}
				// set mask to 0 where class == person
				uint8_t val = (maxpos==pers ? 0 : 255);
				out[n] = (val & 0xE0) | (out[n] >> 3);
			}
			break;
		case modeltype_t::BodyPix:
		case modeltype_t::MLKitSelfie:
			// threshold probability
			for (unsigned int n = 0; n < ctx.output.total(); n++) {
				// FIXME: hardcoded threshold
				uint8_t val = (tmp[n] > 0.65 ? 0 : 255);
				out[n] = (val & 0xE0) | (out[n] >> 3);
			}
			break;
		case modeltype_t::GoogleMeetSegmentation:
			/* 256 x 144 x 2 tensor for the full model or 160 x 96 x 2
			 * tensor for the light model with masks for background
			 * (channel 0) and person (channel 1) where values are in
			 * range [MIN_FLOAT, MAX_FLOAT] and user has to apply
			 * softmax across both channels to yield foreground
			 * probability in [0.0, 1.0]. */
			for (unsigned int n = 0; n < ctx.output.total(); n++) {
				float exp0 = expf(tmp[2*n  ]);
				float exp1 = expf(tmp[2*n+1]);
				float p0 = exp0 / (exp0+exp1);
				float p1 = exp1 / (exp0+exp1);
				uint8_t val = (p0 < p1 ? 0 : 255);
				out[n] = (val & 0xE0) | (out[n] >> 3);
			}
			break;
		case modeltype_t::Unknown:
			_dbg(ctx, "error: unknown model type (%d)\n", ctx.modeltype);
			return false;
	}

	if (ctx.onmask)
		ctx.onmask(ctx.caller_ctx);

	// scale up into full-sized mask
	cv::Mat tmpbuf;
	cv::resize(ctx.ofinal(ctx.in_roidim),tmpbuf,ctx.mroi.size());

	// blur at full size for maximum smoothness
	cv::blur(tmpbuf,ctx.mroi,ctx.blur);

	// copy out
	mask = ctx.mask;
	return true;
}

