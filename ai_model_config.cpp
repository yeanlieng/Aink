#include "ai_model_config.h"

#include <string.h>

struct AiProviderModels {
  const char *const *ids;
  const char *const *labels;
  int count;
};

static const char *kOpenAiIds[] = {
    "gpt-4o-mini",
    "gpt-4o",
    "gpt-4.1-mini",
};

static const char *kOpenAiLabels[] = {
    "4o-mini",
    "4o",
    "4.1-mini",
};

static const char *kGeminiIds[] = {
    "gemini-2.0-flash",
    "gemini-1.5-flash",
};

static const char *kGeminiLabels[] = {
    "2.0-flash",
    "1.5-flash",
};

static const char *kKimiIds[] = {
    "kimi-k2.5",
    "kimi-k2.6",
    "moonshot-v1-8k-vision-preview",
};

static const char *kKimiLabels[] = {
    "k2.5",
    "k2.6",
    "v1-8k-vis",
};

static const char *kMimoIds[] = {
    "mimo-v2.5-pro",
    "mimo-v2.5",
    "mimo-v2-omni",
};

static const char *kMimoLabels[] = {
    "v2.5-pro",
    "v2.5",
    "v2-omni",
};

static const AiProviderModels kProviderModels[AI_PROVIDER_COUNT] = {
    {kOpenAiIds, kOpenAiLabels, 3},
    {kGeminiIds, kGeminiLabels, 2},
    {kKimiIds, kKimiLabels, 3},
    {kMimoIds, kMimoLabels, 3},
};

static const char *kProviderNames[AI_PROVIDER_COUNT] = {
    "OpenAI",
    "Gemini",
    "Kimi Platform",
    "MiMo Token Plan",
};

static const char *kChatCompletionsUrls[AI_PROVIDER_COUNT] = {
    "https://api.openai.com/v1/chat/completions",
    "",
    "https://api.moonshot.ai/v1/chat/completions",
    "https://api.xiaomimimo.com/v1/chat/completions",
};

const char *ai_provider_name(AiProvider provider) {
  if ((unsigned)provider >= AI_PROVIDER_COUNT) {
    return "OpenAI";
  }
  return kProviderNames[provider];
}

int ai_provider_model_count(AiProvider provider) {
  if ((unsigned)provider >= AI_PROVIDER_COUNT) {
    return 0;
  }
  return kProviderModels[provider].count;
}

const char *ai_provider_model_id(AiProvider provider, int modelIndex) {
  if ((unsigned)provider >= AI_PROVIDER_COUNT) {
    return "";
  }
  const AiProviderModels *models = &kProviderModels[provider];
  if (modelIndex < 0 || modelIndex >= models->count) {
    return "";
  }
  return models->ids[modelIndex];
}

const char *ai_provider_model_label(AiProvider provider, int modelIndex) {
  if ((unsigned)provider >= AI_PROVIDER_COUNT) {
    return "--";
  }
  const AiProviderModels *models = &kProviderModels[provider];
  if (modelIndex < 0 || modelIndex >= models->count) {
    return "--";
  }
  return models->labels[modelIndex];
}

const char *ai_provider_chat_completions_url(AiProvider provider) {
  if ((unsigned)provider >= AI_PROVIDER_COUNT) {
    return "";
  }
  return kChatCompletionsUrls[provider];
}

bool ai_provider_supports_vision(AiProvider provider) {
  switch (provider) {
    case AI_PROVIDER_OPENAI:
    case AI_PROVIDER_GEMINI:
    case AI_PROVIDER_KIMI:
    case AI_PROVIDER_MIMO:
      return true;
    default:
      return false;
  }
}

bool ai_provider_model_supports_vision(AiProvider provider, int modelIndex) {
  if (!ai_provider_supports_vision(provider)) {
    return false;
  }
  if (provider == AI_PROVIDER_MIMO) {
    const char *id = ai_provider_model_id(provider, modelIndex);
    return strcmp(id, "mimo-v2.5") == 0 || strcmp(id, "mimo-v2-omni") == 0;
  }
  if (provider == AI_PROVIDER_KIMI) {
    const char *id = ai_provider_model_id(provider, modelIndex);
    return strcmp(id, "moonshot-v1-8k-vision-preview") == 0;
  }
  return true;
}
