#pragma once

#include<iostream>
#include<Eigen/Dense>
#include<Eigen/LU>	//for .inverse().  Probably not needed
#include<vector>
#include<array>
#include<memory>
#include<functional>

namespace LindaleRaster
{


//This is the framebuffer class.  It's a part of namespace LindaleRaster because the LindaleRaster needs to have a well-defined image class to render to.
//It is templated because the output type need not be only colors, could contain anything (like a stencil buffer or depth buffer or gbuffer for deferred rendering)
template<class PixelType>
class Framebuffer
{
protected:
	std::vector<PixelType> data;
public:
	const std::size_t width;
	const std::size_t height;
	//constructor initializes the array
	Framebuffer(std::size_t w,std::size_t h,const PixelType& pt=PixelType()):
		data(w*h,pt),
		width(w),height(h)
	{}
	//2D pixel access
	PixelType& operator()(std::size_t x,std::size_t y)
	{
		return data[y*width+x];
	}
	//const version
	const PixelType& operator()(std::size_t x,std::size_t y) const
	{
		return data[y*width+x];
	}
	void clear(const PixelType& pt=PixelType())
	{
		std::fill(data.begin(),data.end(),pt);
	}
};

//This function runs the vertex shader on all the vertices, producing the varyings that will be interpolated by the rasterizer.
//Vert can be anything, VertShaderOut MUST have a position() method that returns a 4D vector, and it must have an overloaded *= and += operator for the interpolation
//The right way to think of VertShaderOut is that it is the class you write containing the varying outputs from the vertex shader.
template<class Vert,class VertShaderOut,class VertShader>
void run_vertex_shader(const Vert* b,const Vert* e,VertShaderOut* o,
	VertShader vertex_shader)
{
	std::size_t n=e-b;
	#pragma omp parallel for
	for(std::size_t i=0;i<n;i++)
	{
		o[i]=vertex_shader(b[i]);
	}
}
struct BarycentricTransform
{
private:
	Eigen::Vector2f offset;
	Eigen::Matrix2f Ti;
public:
	BarycentricTransform(const Eigen::Vector2f& s1,const Eigen::Vector2f& s2,const Eigen::Vector2f& s3):
		offset(s3)
	{
		Eigen::Matrix2f T;
		T << (s1-s3),(s2-s3);
		Ti=T.inverse();
	}
	Eigen::Vector3f operator()(const Eigen::Vector2f& v) const
	{
		Eigen::Vector2f b;
		b=Ti*(v-offset);
		return Eigen::Vector3f(b[0],b[1],1.0f-b[0]-b[1]);
	}
};
//This function takes in 3 varyings vertices from the fragment shader that make up a triangle,
//rasterizes the triangle and runs the fragment shader on each resulting pixel.
template<class PixelOut,class VertShaderOut,class FragShader>
void rasterize_triangle(Framebuffer<PixelOut>& fb,const std::array<VertShaderOut,3>& verts,FragShader fragment_shader)
{
	std::array<Eigen::Vector4f,3> points{{verts[0].position(),verts[1].position(),verts[2].position()}};
	//Do the perspective divide by w to get screen space coordinates.
	std::array<Eigen::Vector4f,3> epoints{{points[0]/points[0][3],points[1]/points[1][3],points[2]/points[2][3]}};
	auto ss1=epoints[0].head<2>().array(),ss2=epoints[1].head<2>().array(),ss3=epoints[2].head<2>().array();

	//calculate the bounding box of the triangle in screen space floating point.
	Eigen::Array2f bb_ul=ss1.min(ss2).min(ss3);
	Eigen::Array2f bb_lr=ss1.max(ss2).max(ss3);
	Eigen::Array2i isz(fb.width,fb.height);	

	//convert bounding box to fixed point.
	//move bounding box from (-1.0,1.0)->(0,imgdim)
	Eigen::Array2i ibb_ul=((bb_ul*0.5f+0.5f)*isz.cast<float>()).cast<int>();	
	Eigen::Array2i ibb_lr=((bb_lr*0.5f+0.5f)*isz.cast<float>()).cast<int>();
	ibb_lr+=1;	//add one pixel of coverage

	//clamp the bounding box to the framebuffer size if necessary (this is clipping.  Not quite how the GPU actually does it but same effect sorta).
	ibb_ul=ibb_ul.max(Eigen::Array2i(0,0));
	ibb_lr=ibb_lr.min(isz);
	

	BarycentricTransform bt(ss1.matrix(),ss2.matrix(),ss3.matrix());

	//for all the pixels in the bounding box
	for(int y=ibb_ul[1];y<ibb_lr[1];y++)
	for(int x=ibb_ul[0];x<ibb_lr[0];x++)
	{
		Eigen::Vector2f ssc(x,y);
		ssc.array()/=isz.cast<float>();	//move pixel to relative coordinates
		ssc.array()-=0.5f;
		ssc.array()*=2.0f;

		//Compute barycentric coordinates of the pixel center
		Eigen::Vector3f bary=bt(ssc);
		
		//if the pixel has valid barycentric coordinates, the pixel is in the triangle
		if((bary.array() < 1.0f).all() && (bary.array() > 0.0f).all())
		{
			float d=bary[0]*epoints[0][2]+bary[1]*epoints[1][2]+bary[2]*epoints[2][2];
			//Reference the current pixel at that coordinate
			PixelOut& po=fb(x,y);
			// if the interpolated depth passes the depth test
			if(po.depth() < d && d < 1.0)
			{
				//interpolate varying parameters
				VertShaderOut v=verts[0];
				v*=bary[0];
				VertShaderOut vt=verts[1];
				vt*=bary[1];
				v+=vt;
				vt=verts[2];
				vt*=bary[2];
				v+=vt;
				
				//call the fragment shader
				po=fragment_shader(v);
				po.depth()=d; //write the depth buffer
			}
		}
	}
}


//This function rasterizes a set of triangles determined by an index buffer and a buffer of output verts.
template<class PixelOut,class VertShaderOut,class FragShader>
void rasterize(Framebuffer<PixelOut>& fb,const std::size_t* ib,const std::size_t* ie,const VertShaderOut* verts,
	FragShader fragment_shader)
{
	std::size_t n=ie-ib;
	#pragma omp parallel for
	for(std::size_t i=0;i<n;i+=3)
	{
		const std::size_t* ti=ib+i;
		std::array<VertShaderOut,3> tri{{verts[ti[0]],verts[ti[1]],verts[ti[2]]}};
		rasterize_triangle(fb,tri,fragment_shader);
	}
}

//This function does a draw call from an indexed buffer
template<class PixelOut, class VertShaderOut, class Vert, class VertShader, class FragShader>
void draw(	Framebuffer<PixelOut>& fb,
            const std::vector<Vert>* vertexbuffer,
            const std::vector<int>* facebuffer,
		    VertShader vertex_shader,
		    FragShader fragment_shader)
    {
        std::vector<VertShaderOut> vertShaders;
        vertShaders.resize(vertexbuffer->size());
	    run_vertex_shader(vertexbuffer, vertShaders, vertex_shader);
	    rasterize(fb, facebuffer, vertShaders, fragment_shader);
    }

}

#endif
