

int foo(){
	int y = 0;
	int x = 0;
	[[clang::wcet_loop(2)]]
	for(int i = 0; i < 10; i++){
		for(int j = 0; j < 10; j++){
			y *= j;
		}
		x *= i;
	}
	return x;
}
int main(){
	int y = 0;
	for(int i = 0; i < 10; i++){
		[[clang::wcet_loop(1)]]
		for(int j = 0; j < 10; j++){
			y *= j;
		}
		y *= i;
	}
	int x = foo();
	return 0;
}