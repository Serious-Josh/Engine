#pragma once

class Pipeline;
class Texture;

struct Material
{
    Pipeline* pipeline = nullptr;
    Texture* texture = nullptr;
};