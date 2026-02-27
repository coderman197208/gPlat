
#include <cstdio>
#include <iostream>

#include "../include/higplat.h"

extern thread_local int serverHandle;

struct DemoTag
{
	int b;
	int a;

	char order_no[16];				    //合同号
	char melt_no[16];					//炉号
	char lot_no[8];						//试批号
	char roll_no[8];					//轧批号
	char comment[20];					//备注
};

struct TubeInfo
{
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 8)]
	char tube_no[8];                  //管号
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]
	char order_no[16];				    //合同号
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]
	char melt_no[16];					//炉号
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 8)]
	char lot_no[8];					//试批号
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 8)]
	char roll_no[8];					//轧批号
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 20)]
	char comment[20];					//备注
};

void bool1_CHANGED(void* pdata, int datasize)
{
	int b = *((int*)pdata);
	if (b == 0)
		b = 1;
	else
		b = 0;
	unsigned int error;
	writeb(serverHandle, "bool1", &b, sizeof(b), &error);
}

void HHL_POS_CHANGED(void* pdata, int datasize)
{
	TubeInfo*  pTubeInfo = (TubeInfo*)pdata;
	int rowcount = datasize / sizeof(TubeInfo);
	pTubeInfo += 82;
}

void BOOL1_UP(void* pdata, int datasize)
{
	unsigned int error;
	write_plc_int(serverHandle, "DINT1", 1000, &error);
}

void BOOL1_UP_DELAY_2000(void* pdata, int datasize)
{
	unsigned int error;
	write_plc_int(serverHandle, "DINT1", 10000, &error);
}

void BOOL1_DOWN(void* pdata, int datasize)
{
	unsigned int error;
	write_plc_int(serverHandle, "DINT1", 2000, &error);
}

void BOOL1_DOWN_DELAY_2000(void* pdata, int datasize)
{
	unsigned int error;
	write_plc_int(serverHandle, "DINT1", 20000, &error);
}

void DINT1_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT2", a, &error);
}

void DINT2_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT3", a, &error);
}

void DINT3_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT4", a, &error);
}

void DINT4_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT5", a, &error);
}

void DINT5_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT6", a, &error);
}

void DINT6_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT7", a, &error);
}

void DINT7_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT8", a, &error);
}

void DINT8_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT9", a, &error);
}

void DINT9_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT10", a, &error);
}

void DINT10_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT11", a, &error);
}

void DINT11_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT12", a, &error);
}

void DINT12_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT13", a, &error);
}

void DINT13_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT14", a, &error);
}

void DINT14_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT15", a, &error);
}

void DINT15_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT16", a, &error);
}

void DINT16_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT17", a, &error);
}

void DINT17_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT18", a, &error);
}

void DINT18_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT19", a, &error);
}

void DINT19_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT20", a, &error);
}

void DINT20_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT21", a, &error);
}

void DINT21_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT22", a, &error);
}

void DINT22_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT23", a, &error);
}

void DINT23_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT24", a, &error);
}

void DINT24_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT25", a, &error);
}

void DINT25_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT26", a, &error);
}

void DINT26_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT27", a, &error);
}

void DINT27_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT28", a, &error);
}

void DINT28_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT29", a, &error);
}

void DINT29_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT30", a, &error);
}

void DINT30_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT31", a, &error);
}

void DINT31_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT32", a, &error);
}

void DINT32_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT33", a, &error);
}

void DINT33_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT34", a, &error);
}

void DINT34_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT35", a, &error);
}

void DINT35_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT36", a, &error);
}

void DINT36_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT37", a, &error);
}

void DINT37_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT38", a, &error);
}

void DINT38_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT39", a, &error);
}

void DINT39_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT40", a, &error);
}

void DINT40_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT41", a, &error);
}

void DINT41_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT42", a, &error);
}

void DINT42_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT43", a, &error);
}

void DINT43_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT44", a, &error);
}

void DINT44_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT45", a, &error);
}

void DINT45_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT46", a, &error);
}

void DINT46_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT47", a, &error);
}

void DINT47_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT48", a, &error);
}

void DINT48_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT49", a, &error);
}

void DINT49_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT50", a, &error);
}

void DINT50_CHANGED(void* pdata, int datasize)
{
	unsigned int error;
	int a = *(int*)pdata;
	a++;
	write_plc_int(serverHandle, "DINT1", a, &error);
}

void TIMER_FUNCTION(void* pdata, int datasize)
{
	unsigned int error;
	int a = 0;
	write_plc_int(serverHandle, "DINT1", a, &error);
}

void DemoTag00_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag00->a = " << pDemoTag->a << std::endl;
	std::cout << "DemoTag00->b = " << pDemoTag->b << std::endl;
}

void DemoTag01_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag01->a = " << pDemoTag->a << std::endl;
}

void DemoTag02_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag02->a = " << pDemoTag->a << std::endl;
}

void DemoTag03_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag03->a = " << pDemoTag->a << std::endl;
}

void DemoTag04_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag04->a = " << pDemoTag->a << std::endl;
}
void DemoTag05_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag05->a = " << pDemoTag->a << std::endl;
}
void DemoTag06_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag06->a = " << pDemoTag->a << std::endl;
}
void DemoTag07_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag07->a = " << pDemoTag->a << std::endl;
}
void DemoTag08_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag08->a = " << pDemoTag->a << std::endl;
}
void DemoTag09_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag09->a = " << pDemoTag->a << std::endl;
}
void DemoTag10_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag10->a = " << pDemoTag->a << std::endl;
}
void DemoTag11_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag11->a = " << pDemoTag->a << std::endl;
}
void DemoTag12_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag12->a = " << pDemoTag->a << std::endl;
}
void DemoTag13_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag13->a = " << pDemoTag->a << std::endl;
}
void DemoTag14_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag14->a = " << pDemoTag->a << std::endl;
}
void DemoTag15_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag15->a = " << pDemoTag->a << std::endl;
}
void DemoTag16_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag16->a = " << pDemoTag->a << std::endl;
}
void DemoTag17_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag17->a = " << pDemoTag->a << std::endl;
}
void DemoTag18_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag18->a = " << pDemoTag->a << std::endl;
}
void DemoTag19_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag19->a = " << pDemoTag->a << std::endl;
}
void DemoTag20_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag20->a = " << pDemoTag->a << std::endl;
}
void DemoTag21_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag21->a = " << pDemoTag->a << std::endl;
}
void DemoTag22_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag22->a = " << pDemoTag->a << std::endl;
}
void DemoTag23_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag23->a = " << pDemoTag->a << std::endl;
}
void DemoTag24_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag24->a = " << pDemoTag->a << std::endl;
}
void DemoTag25_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag25->a = " << pDemoTag->a << std::endl;
}
void DemoTag26_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag26->a = " << pDemoTag->a << std::endl;
}
void DemoTag27_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag27->a = " << pDemoTag->a << std::endl;
}
void DemoTag28_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag28->a = " << pDemoTag->a << std::endl;
}
void DemoTag29_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag29->a = " << pDemoTag->a << std::endl;
}
void DemoTag30_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag30->a = " << pDemoTag->a << std::endl;
}
void DemoTag31_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag31->a = " << pDemoTag->a << std::endl;
}
void DemoTag32_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag32->a = " << pDemoTag->a << std::endl;
}
void DemoTag33_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag33->a = " << pDemoTag->a << std::endl;
}
void DemoTag34_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag34->a = " << pDemoTag->a << std::endl;
}
void DemoTag35_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag35->a = " << pDemoTag->a << std::endl;
}
void DemoTag36_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag36->a = " << pDemoTag->a << std::endl;
}
void DemoTag37_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag37->a = " << pDemoTag->a << std::endl;
}
void DemoTag38_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag38->a = " << pDemoTag->a << std::endl;
}
void DemoTag39_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag39->a = " << pDemoTag->a << std::endl;
}
void DemoTag40_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag40->a = " << pDemoTag->a << std::endl;
}
void DemoTag41_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag41->a = " << pDemoTag->a << std::endl;
}
void DemoTag42_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag42->a = " << pDemoTag->a << std::endl;
}
void DemoTag43_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag43->a = " << pDemoTag->a << std::endl;
}
void DemoTag44_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag44->a = " << pDemoTag->a << std::endl;
}
void DemoTag45_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag45->a = " << pDemoTag->a << std::endl;
}
void DemoTag46_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag46->a = " << pDemoTag->a << std::endl;
}
void DemoTag47_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag47->a = " << pDemoTag->a << std::endl;
}
void DemoTag48_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag48->a = " << pDemoTag->a << std::endl;
}
void DemoTag49_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag49->a = " << pDemoTag->a << std::endl;
}
void DemoTag50_CHANGED(void* pdata, int datasize)
{
	DemoTag* pDemoTag = (DemoTag *)pdata;
	std::cout << "DemoTag50->a = " << pDemoTag->a << std::endl;
}
