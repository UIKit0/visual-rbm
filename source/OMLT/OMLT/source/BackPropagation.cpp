// std
#include <algorithm>
using std::swap;
#include <assert.h>
#include <random>

// OMLT
#include "BackPropagation.h"

namespace OMLT
{

	BackPropagation::BackPropagation( const ModelConfig in_config, uint32_t in_minibatchsize )
		: _input_units(in_config.InputCount)
		, _minibatch_size(in_minibatchsize)
		, _recompile_required(true)
	{
		assert(in_config.LayerConfigs.size() > 0);
		for(auto it = in_config.LayerConfigs.begin(); it < in_config.LayerConfigs.end(); ++it)
		{
			add_layer(*it);
		}
	}

	BackPropagation::BackPropagation( MultilayerPerceptron* in_mlp, uint32_t in_minibatchsize )
		: _input_units(in_mlp->InputLayer()->inputs)
		, _minibatch_size(in_minibatchsize)
		, _recompile_required(true)
	{
		for(uint32_t k = 0; k < in_mlp->LayerCount(); k++)
		{
			MLP::Layer* layer = in_mlp->GetLayer(k);
			
			LayerConfig layer_config;
			{
				layer_config.Function = layer->function;
				layer_config.InputDropoutProbability = 0.0f;
				layer_config.Noisy = false;
				layer_config.OutputUnits = layer->outputs;
			}

			// allocate contiguous memory block to put into texture memory
			float* weight_buffer = new float[(layer->inputs + 1) * layer->outputs];

			// copy weights from this layer in the MLP to the buffer
			for(uint32_t j = 0; j < layer->outputs; j++)
			{
				weight_buffer[j * (layer->inputs + 1)] = layer->biases[j];
				for(uint32_t i = 1; i <= layer->inputs; i++)
				{
					weight_buffer[j * (layer->inputs + 1) + i] = layer->weights[j][i];
				}
			}

			// add layer and use weight buffer as initial weights
			add_layer(layer_config, weight_buffer);
			delete[] weight_buffer;
		}
	}

	BackPropagation::~BackPropagation()
	{

	}

	void BackPropagation::add_layer( LayerConfig config )
	{
		add_layer(config, nullptr);
	}

	void BackPropagation::add_layer( LayerConfig config, float* weights )
	{
		_layers.push_back(BuildLayer(config, weights));
	}

	void BackPropagation::SetTrainingConfig( const TrainingConfig& in_config)
	{
		// only set recompile flag if the new config is different
		if(memcmp(&in_config, &_training_config, sizeof(TrainingConfig)) != 0)
		{
			_training_config = in_config;
			_recompile_required = true;
		}
	}

	float BackPropagation::Train( OpenGLBuffer2D& example_input, OpenGLBuffer2D& example_label )
	{
		if(_recompile_required)
		{
			free_kernels();
			build_kernels();

			_recompile_required = false;
		}

		assert(example_input.Width == _input_units);
		assert(example_input.Height == _minibatch_size);
		assert(example_label.Width == _layers.back()->OutputUnits);
		assert(example_label.Height == _minibatch_size);

		// feed forward
		_layers.front()->Input = &example_input;
		for(auto it = _layers.begin(); it != _layers.end(); ++it)
		{
			Layer* lay = *it;

			OpenGLProgram* calc_enabled = lay->CalcEnabledInputs;
			{
				calc_enabled->SetInput(0, lay->InputRandom0);
				assert(lay->InputRandom0.Width == lay->InputUnits);
				assert(lay->InputRandom0.Height == 1);

				calc_enabled->BindOutput(0, lay->InputRandom1);
				calc_enabled->BindOutput(1, lay->InputEnabled);
				assert(lay->InputRandom1.Width == lay->InputUnits);
				assert(lay->InputRandom1.Height == 1);
				assert(lay->InputEnabled.Width == lay->InputUnits);
				assert(lay->InputEnabled.Height == 1);

				calc_enabled->Run();

				swap(lay->InputRandom0, lay->InputRandom1);
			}

			OpenGLProgram* feed_forward = lay->FeedForward;
			{
				feed_forward->SetInput(0, *lay->Input);
				feed_forward->SetInput(1, lay->InputEnabled);
				feed_forward->SetInput(2, lay->Weights0);
				feed_forward->SetInput(3, lay->OutputRandom0);
				assert(lay->Input->Width == lay->InputUnits);
				assert(lay->Input->Height == _minibatch_size);
				assert(lay->Weights0.Width == (lay->InputUnits + 1));
				assert(lay->Weights0.Height == lay->OutputUnits);
				assert(lay->OutputRandom0.Width == lay->OutputUnits);
				assert(lay->OutputRandom0.Height == _minibatch_size);

				feed_forward->BindOutput(0, lay->Activation);
				feed_forward->BindOutput(1, lay->OutputRandom1);
				assert(lay->Activation.Width == lay->OutputUnits);
				assert(lay->Activation.Height == _minibatch_size);
				assert(lay->OutputRandom1.Width == lay->OutputRandom0.Width);
				assert(lay->OutputRandom1.Height == lay->OutputRandom0.Height);

				feed_forward->Run();

				swap(lay->OutputRandom0, lay->OutputRandom1);
			}
		}

		// error calculation
		float err = 0.0f;
		{
			static float* calculate_output = nullptr;
			static float* desired_output = nullptr;

			_layers.back()->Activation.GetData(calculate_output);
			example_label.GetData(desired_output);

			uint32_t pixels = example_label.Width * example_label.Height;

			for(uint32_t i = 0; i < pixels; i++)
			{
				float diff = calculate_output[i] - desired_output[i];
				err += diff*diff;
			}

			err /= pixels;
		}
		// calc sensitivities, feed backward
		{
			auto it = _layers.rbegin();
			Layer* lay = *it;
			OpenGLProgram* calc_sensitivities = lay->CalcSensitivity;
			{
				// fill out calc_top_sensitivities (and set the training examples the labels!
				calc_sensitivities->SetInput(0, example_label);
				calc_sensitivities->SetInput(1, lay->Activation);
				assert(example_label.Width == lay->OutputUnits);
				assert(example_label.Height == _minibatch_size);
				assert(lay->Activation.Width == lay->OutputUnits);
				assert(lay->Activation.Height == _minibatch_size);
			
				calc_sensitivities->BindOutput(0, lay->Sensitivities);
				assert(lay->Sensitivities.Width == lay->OutputUnits);
				assert(lay->Sensitivities.Height == _minibatch_size);

				calc_sensitivities->Run();

				//float* sensitivities = nullptr;
				//lay->Sensitivities.GetData(sensitivities);

				//printf("");
			}
			while(++it != _layers.rend())
			{
				lay = *it;
				// fill out whatever
				calc_sensitivities = lay->CalcSensitivity;

				calc_sensitivities->SetInput(0, lay->NextLayer->Weights0);
				calc_sensitivities->SetInput(1, lay->NextLayer->Sensitivities);
				calc_sensitivities->SetInput(2, lay->Activation);
				assert(lay->NextLayer->Weights0.Width == (lay->OutputUnits + 1));

				calc_sensitivities->BindOutput(0, lay->Sensitivities);
				assert(lay->Sensitivities.Width == lay->OutputUnits);
				assert(lay->Sensitivities.Height == _minibatch_size);

				calc_sensitivities->Run();
			}
		}

		// now update all the weight matrices
		for(auto it = _layers.rbegin(); it != _layers.rend(); ++it)
		{
			Layer* lay = *it;
			OpenGLProgram* update_weights = lay->UpdateWeights;

			// fill out whatever		
			update_weights->SetInput(0, lay->Sensitivities);
			update_weights->SetInput(1, *lay->Input);
			update_weights->SetInput(2, lay->InputEnabled);
			update_weights->SetInput(3, *lay->OutputEnabled);
			update_weights->SetInput(4, lay->Weights0);
			update_weights->SetInput(5, lay->DeltaWeights0);
			assert(lay->Sensitivities.Width == lay->OutputUnits);
			assert(lay->Sensitivities.Height == _minibatch_size);
			assert(lay->Input->Width == lay->InputUnits);
			assert(lay->Input->Height == _minibatch_size);
			assert(lay->InputEnabled.Width == lay->InputUnits);
			assert(lay->InputEnabled.Height == 1);
			assert(lay->OutputEnabled->Width == lay->OutputUnits);
			assert(lay->OutputEnabled->Height == 1);
			assert(lay->Weights0.Width == (lay->InputUnits + 1));
			assert(lay->Weights0.Height == lay->OutputUnits);
			assert(lay->DeltaWeights0.Width == lay->Weights0.Width);
			assert(lay->DeltaWeights0.Height == lay->Weights0.Height);
		
			update_weights->BindOutput(0, lay->Weights1);
			update_weights->BindOutput(1, lay->DeltaWeights1);
			assert(lay->Weights1.Width == (lay->InputUnits + 1));
			assert(lay->Weights1.Height == lay->OutputUnits);
			assert(lay->DeltaWeights1.Width == lay->Weights1.Width);
			assert(lay->DeltaWeights1.Height == lay->Weights1.Height);


			update_weights->Run();

			//float* delta_weights = nullptr;
			//float* weights = nullptr;
			//lay->DeltaWeights1.GetData(delta_weights);
			//lay->Weights1.GetData(weights);

			swap(lay->Weights0, lay->Weights1);
			swap(lay->DeltaWeights0, lay->DeltaWeights1);
		}

		return err;
	}


	BackPropagation::Layer::Layer()
	{

	}

	BackPropagation::Layer::~Layer()
	{

	}

	BackPropagation::Layer* BackPropagation::BuildLayer( LayerConfig in_Config, float* in_weights )
	{
		std::mt19937_64 random;
		random.seed(1);
		std::uniform_int_distribution<uint32_t> uniform(0, 0xFFFFFFFF);
		std::normal_distribution<float> normal;

		Layer* result = new Layer();

		if(_layers.size() == 0)
		{
			result->InputUnits = this->_input_units;
		}
		else
		{
			result->InputUnits = _layers.back()->OutputUnits;
		}
		result->OutputUnits = in_Config.OutputUnits;
	
		result->Function = in_Config.Function;
		result->Noisy = in_Config.Noisy;
		result->InputDropoutProbability = in_Config.InputDropoutProbability;

		if(_layers.size() == 0)
		{
			result->Input = nullptr;
		}
		else
		{
			result->Input = &_layers.back()->Activation;
		}

		uint32_t width, height;
		// init input random
		{
			width = result->InputUnits;
			height = 1;

			result->InputEnabled = OpenGLBuffer2D(width, height, ReturnType::Float, nullptr);

			uint32_t* random_buffer = new uint32_t[width * height];
			for(uint32_t i = 0; i < width * height; i++)
			{
				random_buffer[i] = uniform(random);
			}
			result->InputRandom0 = OpenGLBuffer2D(width, height, ReturnType::UInt, random_buffer);
			delete[] random_buffer;
			result->InputRandom1 = OpenGLBuffer2D(width, height, ReturnType::UInt, nullptr);
		}

		// init weights, delta weights
		// weight format: j rows, each containing i + 1 values, first value in each row is bias
		{
			width = result->InputUnits + 1;
			height = result->OutputUnits;

			
			if(in_weights == nullptr)
			{
				float* weight_buffer = new float[width * height];
		
				for(uint32_t j = 0; j < height; j++)
				{
					// bias is first value in a row
					int i = 0;
					// always init bias to 0
					weight_buffer[j * width + 0] = 0.0f;
					for(i = 1; i < width; i++)
					{
						weight_buffer[j * width + i] = normal(random) * 0.1f;

						//printf("From %i to %i: %f\n", i, j, weight_buffer[j * width + i]);
					}
				}

				result->Weights0 = OpenGLBuffer2D(width, height, ReturnType::Float, weight_buffer);
				delete[] weight_buffer;
			}
			else
			{
				result->Weights0 = OpenGLBuffer2D(width, height, ReturnType::Float, in_weights);
			}
			result->Weights1 = OpenGLBuffer2D(width, height, ReturnType::Float, nullptr);
			result->DeltaWeights0 = OpenGLBuffer2D(width, height, ReturnType::Float, nullptr);
			result->DeltaWeights1 = OpenGLBuffer2D(width, height, ReturnType::Float, nullptr);
			result->OutputEnabled = nullptr;

			if(_layers.size() > 0)
			{
				_layers.back()->OutputEnabled = &result->InputEnabled;
			}
		}

		// now init our output related buffers
		{
			width = result->OutputUnits;
			height = _minibatch_size;
			result->Activation =  OpenGLBuffer2D(width, height, ReturnType::Float, nullptr);
			result->OutputRandom0 =  OpenGLBuffer2D(width, height, ReturnType::UInt, nullptr);
			result->OutputRandom1 =  OpenGLBuffer2D(width, height, ReturnType::UInt, nullptr);
		}

		// sensitivites all on their own
		{
			width = result->OutputUnits;
			height = _minibatch_size;
			result->Sensitivities =  OpenGLBuffer2D(width, height, ReturnType::Float, nullptr);
		}

		result->CalcEnabledInputs = nullptr;
		result->FeedForward = nullptr;
		result->CalcSensitivity = nullptr;
		result->UpdateWeights = nullptr;
	
		result->NextLayer = nullptr;
		if(_layers.size() > 0)
		{
			_layers.back()->NextLayer = result;
		}

		return result;
	}

	MultilayerPerceptron* BackPropagation::GetMultilayerPerceptron() const
	{
		return GetMultilayerPerceptron(0, _layers.size() - 1);
	}

	MultilayerPerceptron* BackPropagation::GetMultilayerPerceptron(uint32_t begin_layer, uint32_t end_layer) const
	{
		assert(begin_layer < _layers.size() && end_layer < _layers.size());
		assert(begin_layer <= end_layer);

		MultilayerPerceptron* result = new MultilayerPerceptron();

		for(uint32_t k = begin_layer; k <= end_layer; k++)
		{
			BackPropagation::Layer* bp_layer = _layers[k];

			MultilayerPerceptron::Layer* layer = new MultilayerPerceptron::Layer(bp_layer->InputUnits, bp_layer->OutputUnits, bp_layer->Function);
			float* gpu_weights = nullptr;
			bp_layer->Weights0.GetData(gpu_weights);

			float* gpu_weights_head = gpu_weights;
			for(uint32_t j = 0; j < layer->outputs; j++)
			{
				layer->biases[j] = gpu_weights_head[0];
				memcpy(layer->weights[j], gpu_weights_head + 1, sizeof(float) * layer->inputs);

				gpu_weights_head += layer->inputs + 1;
			}

			bool added = result->AddLayer(layer);
			assert(added == true);

			free(gpu_weights);
		}

		return result;
	}

	void BackPropagation::free_kernels()
	{
		for(auto it = _layers.begin(); it < _layers.end(); ++it)
		{
			Layer* layer = *it;
			SafeDelete(layer->CalcEnabledInputs);
			SafeDelete(layer->FeedForward);
			SafeDelete(layer->CalcSensitivity);
			SafeDelete(layer->UpdateWeights);
		}
	}

	void BackPropagation::build_kernels()
	{
		OpenGLCompiler comp;

		for(auto it = _layers.begin(); it != _layers.end(); ++it)
		{
			Layer* layer = *it;

			// calc enabled inputs
			{
				SourceCalcEnabledUnits source;
				source.DROPOUT_PROB = layer->InputDropoutProbability;

				source.Parse();
				layer->CalcEnabledInputs = comp.Build(source);
				layer->CalcEnabledInputs->Initialize(layer->InputUnits, 1);
				//printf("%s\n", layer->CalcEnabledInputs->GetSource().c_str());
			}

			// feed forward
			{
				SourceFeedForward source;
				source.FUNC = layer->Function;
				source.INPUT_DROPOUT_PROB = layer->InputDropoutProbability;
				source.INPUT_COUNT = layer->InputUnits;
				source.NOISY = layer->Noisy;

				source.Parse();
				layer->FeedForward = comp.Build(source);
				layer->FeedForward->Initialize(layer->OutputUnits, _minibatch_size);
				//printf("%s\n", layer->FeedForward->GetSource().c_str());
			}

			// calc sensitivies
			{
				if(layer == _layers.back())
				{
					SourceCalcTopSensitivities source;
					source.FUNC = layer->Function;
					source.MINIBATCH_SIZE = _minibatch_size;

					source.Parse();
					layer->CalcSensitivity = comp.Build(source);
					layer->CalcSensitivity->Initialize(layer->OutputUnits, _minibatch_size);
					//printf("%s\n", layer->CalcSensitivity->GetSource().c_str());
				}
				else
				{
					SourceCalcSensitivities source;
					source.FUNC = layer->Function;
					source.MINIBATCH_SIZE = _minibatch_size;
					source.NEXT_OUTPUT_COUNT = layer->NextLayer->OutputUnits;

					source.Parse();
					layer->CalcSensitivity = comp.Build(source);
					layer->CalcSensitivity->Initialize(layer->OutputUnits, _minibatch_size);
					//printf("%s\n", layer->CalcSensitivity->GetSource().c_str());
				}
			}

			// update weights
			{
				SourceUpdateWeights source;
				source.LEARNING_RATE = _training_config.LearningRate;
				source.MOMENTUM = _training_config.Momentum;
				source.MINIBATCH_SIZE = _minibatch_size;
				source.L1_REGULARIZATION = _training_config.L1Regularization;
				source.L2_REGULARIZATION = _training_config.L2Regularization;

				source.Parse();
				layer->UpdateWeights = comp.Build(source);
				layer->UpdateWeights->Initialize(layer->InputUnits + 1, layer->OutputUnits);
				//printf("%s\n", layer->UpdateWeights->GetSource().c_str());
			}
		}	

		{
			Layer* last_layer = _layers.back();
			if(last_layer->OutputEnabled == nullptr)
			{
				float* enabled = new float[last_layer->OutputUnits];
				for(uint32_t j = 0; j < last_layer->OutputUnits; j++)
				{
					enabled[j] = 1.0f;
				}

				last_layer->OutputEnabled = new OpenGLBuffer2D(last_layer->OutputUnits, 1, ReturnType::Float, enabled);
				delete[] enabled;
			}
		}
	}
}