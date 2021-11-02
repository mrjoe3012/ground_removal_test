#include <iostream>
#include <cmath>
#include <vector>
#include <memory>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <thread>

#include <pcl/io/pcd_io.h>
#include <pcl/point_cloud.h>
#include <pcl/visualization/cloud_viewer.h>
#include <pcl/visualization/pcl_visualizer.h>

// essential data structures

// start of vector definitions

struct Vector2
{
	Vector2() : Vector2(0,0) {};
	Vector2(float x, float y) : x(x),y(y) {};
	Vector2(const Vector2& rhs) : x(rhs.x),y(rhs.y) {};

	friend Vector2 operator+(const Vector2& lhs, const Vector2& rhs);
	friend Vector2 operator-(const Vector2& lhs, const Vector2& rhs);
	friend Vector2 operator*(const Vector2& rhs, const float& lhs);
	friend Vector2 operator*(const float& rhs, const Vector2& lhs);
	friend Vector2 operator/(const Vector2& lhs, const float& rhs);
	friend bool operator==(const Vector2& lhs, const Vector2& rhs);

	// convert a 3d point to a 2d point based on its distance
	// from the centre of the 3d space and its height
	template<typename PointT>
	static Vector2 fromPclPoint(const PointT& p) { return Vector2(std::sqrt(std::pow(p.x,2) + std::pow(p.y,2)), p.z); };

	float sqrMagnitude() const { return std::pow(x,2) + std::pow(y,2); };
	float magnitude() const { return std::sqrt(sqrMagnitude()); };

	float x, y;
};

Vector2 operator+(const Vector2& lhs, const Vector2& rhs)
{
	return Vector2(lhs.x+rhs.x, lhs.y+rhs.y);
}

Vector2 operator-(const Vector2& lhs, const Vector2& rhs)
{
	return lhs + (-1*rhs);
}

Vector2 operator*(const Vector2& lhs, const float& rhs)
{
	return Vector2(lhs.x*rhs,lhs.y*rhs);
}

Vector2 operator*(const float& lhs, const Vector2& rhs)
{
	return operator*(rhs, lhs);
}

Vector2 operator/(const Vector2& lhs, const float& rhs)
{
	return (1.0f/rhs) * lhs;
}

bool operator==(const Vector2& lhs, const Vector2& rhs)
{
	return lhs.x==rhs.x && lhs.y==rhs.y;
}

// end of vector definitions

// start of line definitions

struct Line
{
	Line(float gradient, float yIntercept, Vector2 beginPoint, Vector2 endPoint) : gradient(gradient), yIntercept(yIntercept), beginPoint(beginPoint), endPoint(endPoint) {};
	Line(const Line& rhs) : Line(rhs.gradient, rhs.yIntercept, rhs.beginPoint, rhs.endPoint) {};

	float gradient, yIntercept;
	Vector2 beginPoint, endPoint;
};

// end of line definitions

// typedefs

// represents an array of pcl::Pointx pointers, used to represents points within a bin (within a segment)
template<typename PointT>
using PointArray = std::vector<const PointT*>;

// represents an array of bins, belonging to a specific segment
template<typename PointT>
using BinArray = std::vector< PointArray<PointT> >;

// the data structure we will be using to store point->(segment[bin]) assignments.
template<typename PointT>
using SegmentArray = std::vector< BinArray<PointT> >; 

// (smart) pointer to a point cloud
template<typename PointT>
using PCPtr = typename pcl::PointCloud<PointT>::Ptr;

// end of typedefs

// Currently always divides an FOV of 360degrees. This means that FOVs which are not 360degrees or 180degrees will have
// uneven segment assignments. Although given the use-case for the algorithm, it's unlikely that we would ever work with
// an FOV that is not 180degrees. Additionally, converting this function to work with any FOV should be relatively
// trivial.
// Note that we are templating this function (as well as many others) so that they work with a variety of pcl::Point data
// types e.g. pcl::PointXYZ, pcl::PointXYZRGB, ...
template<typename PointT>
std::unique_ptr< SegmentArray< PointT > > assignPointsToBinsAndSegments(const pcl::PointCloud<PointT>& cloud, int numberOfSegments, int numberOfBins)
{
	const float pi = 4*std::atan(1);

	// construct the result so that it already contains the correct number of arrays for segments and bins
	std::unique_ptr< SegmentArray< PointT > > pSegmentArray(new SegmentArray<PointT>(numberOfSegments, BinArray<PointT>(numberOfBins)));	

	// figure out how far away the farthest point is
	// so that we can determine how big each bin should
	// be.
	// Note that using the distance squared is acceptable here
	const PointT& farthestPoint = *std::max_element(cloud.begin(), cloud.end(), [](const PointT& p1, const PointT& p2){ return std::pow(p1.x, 2) + std::pow(p1.y, 2) < std::pow(p2.x, 2) + std::pow(p2.y, 2); });
	float farthestPointDistance = std::sqrt(std::pow(farthestPoint.x,2)+std::pow(farthestPoint.y,2));

	// distance between edge of each bin
	float binRange = farthestPointDistance / numberOfBins;
	// angle between each segment edge
	float segmentAngle = (2.0f*pi) / numberOfSegments;

	for(const PointT& point : cloud)
	{
		float pointDistance = std::sqrt(std::pow(point.x,2)+std::pow(point.y,2));
		float pointAngleWithXAxis = std::atan2(point.y, point.x);
		if(pointAngleWithXAxis < 0)
			pointAngleWithXAxis = 2.0f*pi + pointAngleWithXAxis;

		
		unsigned int segmentIndex = static_cast<unsigned int>(pointAngleWithXAxis/segmentAngle);
		unsigned int binIndex = std::min(static_cast<unsigned int>(pointDistance/binRange), static_cast<unsigned int>(numberOfBins-1));

		// assign the bin according to its index
		(*pSegmentArray)[segmentIndex][binIndex].push_back(&point);
	}


	return pSegmentArray;

}

// fits a line through an array of 2d points
// least squares method from https://web.archive.org/web/20150715022401/http://faculty.cs.niu.edu/~hutchins/csci230/best-fit.htm
Line fitLine2D(const std::vector<Vector2>& points)
{
	float sumX = 0.0f, sumY = 0.0f, sumXX = 0.0f, sumXY = 0.0f;

	for(const Vector2& point : points)
	{
		sumX += point.x;
		sumY += point.y;
		sumXX += point.x*point.x;
		sumXY += point.x*point.y;
	}

	float count = points.size();
	float xMean = sumX/count, yMean = sumY/count;
	float slope = (sumXY - sumX*yMean) / (sumXX - sumX*xMean);
	float yIntercept = yMean - slope*xMean;

	return Line(slope, yIntercept, points.front(), points.back());
}

// returns the minimum distance between a (2d) point and a (2d) line.
float distanceFromPointToLine(const Vector2& point, const Line& line)
{
	Vector2 v1(point.x, point.y);
	Vector2 v2(point.x, point.x*line.gradient + line.yIntercept);
	return (v1-v2).magnitude();
}

// returns the root mean square error of a fit
// https://en.wikipedia.org/wiki/Root-mean-square_deviation
float fitRMSE(const Line& line, const std::vector<Vector2>& points)
{

	float sum = 0;
	float count = points.size();
	
	for(const Vector2& point : points)
	{
		float x = point.x;
		float y = point.y;
		float yExpected = line.gradient*x + line.yIntercept;
		sum += std::pow(yExpected-y, 2);
	}

	return std::sqrt(sum/count);

}

// Gets the prototype point, which is the point with lowest
// z value
template<typename PointT>
Vector2 prototypePoint(const PointArray<PointT>& pointArray)
{
	const PointT* pPoint = *std::min_element(pointArray.begin(), pointArray.end(), [](const PointT* lhs, const PointT* rhs){ return lhs->z < rhs->z; });
	// convert 3d point into 2d point
	Vector2 point2d = Vector2::fromPclPoint(*pPoint);
	return point2d;
}

// returns a series of ground plane line approximations for a specific segment
// (algorithm 1 in "Fast Segmentation of 3D Point Clouds for Gound Vehicles" pg.562)
template<typename PointT>
std::vector<Line> groundPlaneLinesForSegment(const BinArray<PointT>& binArray)
{
	const float tM = 2.0f, tMSmall = -3.0f;
	const float tB = 1.0f;
	const float tRMSE = 0.005f;
	const float tDPrev = 1.0f;

	// points which are incrementally populated and
	// are ultimately used to fit ground plane lines
	std::vector<Vector2> linePoints;
	// the ground plane lines
	std::vector<Line> lines;

	unsigned int numberOfBins = binArray.size();

	for(unsigned int i = 0, c = 0; i < numberOfBins; i++)
	{
		const std::vector<const PointT*>& pointArray = binArray[i];

		if(pointArray.size() > 0)
		{
			Vector2 prototype = prototypePoint(pointArray);
			if(linePoints.size() >= 2)
			{
				linePoints.push_back(prototype);
				Line line = fitLine2D(linePoints);
				std::cout << line.gradient << ", " << line.yIntercept << ", " <<  fitRMSE(line, linePoints) << std::endl;
				if(!(std::abs(line.gradient) <= tM && (line.gradient > tMSmall || std::abs(line.yIntercept) <= tB) && fitRMSE(line, linePoints) <= tRMSE))
				{
					linePoints.pop_back();
					line = fitLine2D(linePoints);
					lines.push_back(line);
					linePoints.clear();
					c++;
					i--;
				}

			}
			else
			{
				if(linePoints.size() != 0 || c == 0 || distanceFromPointToLine(prototype, lines[c-1]) <= tDPrev)
				{
					linePoints.push_back(prototype);
				}
			}
		}

	}

	std::cout << lines.size() << " ground plane lines" << std::endl;

	return lines;

}

// returns a new point cloud with (hopefully) less ground points
// TODO create a structure for the parameter set and allow it to be customised
// per-call, rather than hard-coded as constants.
template<typename PointT>
typename pcl::PointCloud<PointT>::Ptr groundRemoval(const SegmentArray<PointT>& segmentArray)
{

	const float tDGround = 0.175f;

	float numberOfSegments = segmentArray.size();

	typename pcl::PointCloud<PointT>::Ptr result(new pcl::PointCloud<PointT>);

	unsigned int segmentIndex = 0;

	for(const BinArray<PointT>& binArray : segmentArray)
	{
		std::vector<Line> groundPlaneLines = groundPlaneLinesForSegment(binArray);
		for(const PointArray<PointT>& pointArray : binArray)
		{
			for(const PointT* point : pointArray)
			{
				Vector2 point2d = Vector2::fromPclPoint(*point);
				bool groundPoint = false;
				for (const Line& line : groundPlaneLines)
				{
					float distanceToStart = (line.beginPoint - point2d).magnitude();
					float distanceToEnd = (line.endPoint - point2d).magnitude();
					if(distanceToStart <= tDGround || distanceToEnd <= tDGround)
					{
						groundPoint = true;
						break;
					}
				}
				if(!groundPoint)
					result->push_back(*point);
					
			}
		}

		segmentIndex++;

	}	

	return result;

}

int main()
{

	std::srand(std::time(0));

	// get point cloud from .pcd file

	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud1(new pcl::PointCloud<pcl::PointXYZRGB>);

	if(pcl::io::loadPCDFile<pcl::PointXYZRGB>("sample_data/cloud1.pcd", *cloud1) == -1)
	{
		PCL_ERROR("Couldn't read sample data.");
		return -1;
	}

	unsigned int numSegments = 6, numBins = 128;

	std::unique_ptr<SegmentArray<pcl::PointXYZRGB>> pSegmentArray = assignPointsToBinsAndSegments(*cloud1, numSegments, numBins);


	// colour each point according to its segment
	for(BinArray<pcl::PointXYZRGB>& binArray : *pSegmentArray)
	{
		for(PointArray<pcl::PointXYZRGB>& pointArray : binArray)
		{
			float r = 50 + std::rand()%206;
			float g = 50 + std::rand()%206;
			float b = 50 + std::rand()%206;
			for(const pcl::PointXYZRGB* pPoint : pointArray)
			{
				pcl::PointXYZRGB* p = const_cast<pcl::PointXYZRGB*>(pPoint);
				p->r = r;
				p->g = g;
				p->b = b;
			}	
		}
	}

	// get cloud with ground removed
	
	auto cloud2 = groundRemoval(*pSegmentArray);

	// visualize point clouds

	pcl::visualization::CloudViewer viewer("Viewer");

	viewer.showCloud(cloud1);
	bool c1 = true;

	while(!viewer.wasStopped()) 
	{
		using namespace std::chrono_literals;
		std::this_thread::sleep_for(3000ms);
		//viewer.showCloud((c1 ? cloud2 : cloud1));
		c1 = !c1;
	}

	return 0;
}
